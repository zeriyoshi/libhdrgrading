/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_WB_ID        "hdrg.webgpu.deblock"
#define HDRG_WB_INFLIGHT  2u /* frames in flight: the next one's upload meets this one's */
#define HDRG_WB_BINDINGS  3u
#define HDRG_WB_GROUP     8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WB_GRID      8u /* the blocks' grid */
#define HDRG_WB_REACH     4u /* the samples a boundary reads on each side */
#define HDRG_WB_SUBMITTED (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)

/*
 * hdrg.deblock (src/plugins/deblock/deblock.c) on the GPU
 * (src/plugins/deblock/wgsl/deblock.wgsl): the same thresholds from the frame's
 * noise, decisions, filters and passthrough, whole RGB16 frames uploaded,
 * deblocked in two dispatches and read back; the output equals the CPU's.
 * ASYNC: the jobs run in order on the device's thread (src/gpu/webgpu),
 * HDRG_WB_INFLIGHT of them in flight.
 */
typedef struct {
    hdrg_gpu_t *gpu;
    hdrg_webgpu_device_frame_t device[HDRG_WEBGPU_DEVICE_FRAMES]; /* to the next filter */
    WGPUBindGroupLayout group_layout;                             /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline verticals, horizontals;
    WGPUBuffer params, input, output, readback;
    size_t in_size, out_size; /* the buffers' (0: none yet) */
    uint32_t width, height;   /* configure's */
    int32_t alpha, beta, tc;  /* configure's: hdrg.deblock's thresholds */
    bool active;
    bool built; /* create: the pipelines */
    double strength;
} hdrg_wb_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride;
    int32_t alpha, beta, tc, pad;
} hdrg_wb_params_t;

/* a job as submitted: what its run needs (the next frame's configure may come first) */
typedef struct {
    hdrg_wb_t *wb;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    hdrg_webgpu_job_t frames;
    hdrg_wb_params_t params;
} hdrg_wb_work_t;

static void hdrg_wb_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

/* the device's thread: the layouts and the two pipelines */
static void hdrg_wb_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WB_BINDINGS] = {WGPUBufferBindingType_Uniform,
                                                           WGPUBufferBindingType_ReadOnlyStorage,
                                                           WGPUBufferBindingType_Storage};
    hdrg_wb_t *wb = (hdrg_wb_t *)arg;
    WGPUBindGroupLayoutEntry entries[HDRG_WB_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;

    for (i = 0; i < HDRG_WB_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    group_desc.entryCount = HDRG_WB_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    wb->group_layout = wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &wb->group_layout;
    wb->layout = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_deblock);
    if (module == NULL) {
        return;
    }

    wb->verticals = hdrg_gpu_pipeline(gpu, module, "verticals", wb->layout);
    wb->horizontals = hdrg_gpu_pipeline(gpu, module, "horizontals", wb->layout);
    wb->params = hdrg_gpu_buffer(gpu, sizeof(hdrg_wb_params_t),
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);

    wb->built = wb->verticals != NULL && wb->horizontals != NULL && wb->params != NULL;
}

/* the device's thread: every object */
static void hdrg_wb_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wb_t *wb = (hdrg_wb_t *)arg;

    (void)gpu;

    if (wb->verticals != NULL) {
        wgpuComputePipelineRelease(wb->verticals);
    }

    if (wb->horizontals != NULL) {
        wgpuComputePipelineRelease(wb->horizontals);
    }

    if (wb->layout != NULL) {
        wgpuPipelineLayoutRelease(wb->layout);
    }

    if (wb->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(wb->group_layout);
    }

    hdrg_wb_release_buffer(&wb->params);
    hdrg_wb_release_buffer(&wb->input);
    hdrg_wb_release_buffer(&wb->output);
    hdrg_wb_release_buffer(&wb->readback);
    hdrg_webgpu_device_frames_free(wb->device);
}

/* the device's thread: the frame buffers for a job's sizes, kept while they are large enough */
static bool hdrg_wb_buffers(hdrg_gpu_t *gpu, hdrg_wb_t *wb, size_t in_size, size_t out_size)
{
    if (in_size > wb->in_size) {
        hdrg_wb_release_buffer(&wb->input);
        wb->input = hdrg_gpu_buffer(gpu, in_size, HDRG_WB_SUBMITTED);
        wb->in_size = (wb->input != NULL) ? in_size : 0u;
    }

    if (out_size > wb->out_size) {
        hdrg_wb_release_buffer(&wb->output);
        hdrg_wb_release_buffer(&wb->readback);
        wb->output =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        wb->readback =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        wb->out_size = (wb->output != NULL && wb->readback != NULL) ? out_size : 0u;
    }

    return wb->in_size >= in_size && wb->out_size >= out_size;
}

/* the device's thread: a job's upload, dispatches and read-back */
static hdrg_status_t hdrg_wb_frame(hdrg_gpu_t *gpu, const hdrg_wb_work_t *work)
{
    const size_t in_size = work->frames.in_stride * work->params.height,
                 out_size = work->frames.out_stride * work->params.height;
    const void *mapped;
    hdrg_wb_t *wb = work->wb;
    WGPUBindGroupEntry entries[HDRG_WB_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WB_BINDINGS], in_buffer, out_buffer;
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    uint32_t i, width, height;
    bool ok;

    /* (the frames on the device stand in for the input and output buffers: none made) */
    if (!hdrg_wb_buffers(gpu, wb, (work->frames.in_frame != NULL) ? 0u : in_size,
                         (work->frames.out_frame != NULL) ? 0u : out_size)) {
        return HDRG_ERR_NOMEM;
    }

    /* the device frames: the previous filter's for the input, one of its own for the output */
    if (work->frames.out_frame != NULL &&
        !hdrg_webgpu_device_frame_fit(gpu, work->frames.out_frame, out_size)) {
        return HDRG_ERR_NOMEM;
    }

    in_buffer =
        (work->frames.in_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.in_frame) : wb->input;
    out_buffer = (work->frames.out_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.out_frame)
                                                  : wb->output;

    buffers[0] = wb->params;
    buffers[1] = in_buffer;
    buffers[2] = out_buffer;

    for (i = 0; i < HDRG_WB_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = wb->group_layout;
    group_desc.entryCount = HDRG_WB_BINDINGS;
    group_desc.entries = entries;
    width = work->params.width;
    height = work->params.height;

    hdrg_gpu_scope_begin(gpu);

    if (work->frames.in_frame == NULL) {
        wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wb->input, 0, work->frames.in, in_size);
    }

    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wb->params, 0, &work->params, sizeof(work->params));
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderSetPipeline(pass, wb->verticals);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, ((width + HDRG_WB_REACH) / HDRG_WB_GRID + HDRG_WB_GROUP) / HDRG_WB_GROUP,
        (height + HDRG_WB_GROUP - 1u) / HDRG_WB_GROUP, 1);
    wgpuComputePassEncoderSetPipeline(pass, wb->horizontals);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, ((width + 1u) / 2u + HDRG_WB_GROUP - 1u) / HDRG_WB_GROUP,
        (height / HDRG_WB_GRID + HDRG_WB_GROUP) / HDRG_WB_GROUP, 1);
    wgpuComputePassEncoderEnd(pass);

    if (work->frames.out_frame == NULL) {
        wgpuCommandEncoderCopyBufferToBuffer(encoder, wb->output, 0, wb->readback, 0, out_size);
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

    mapped = ok ? hdrg_gpu_map_read(gpu, wb->readback, out_size) : NULL;

    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->frames.out, mapped, out_size);
    wgpuBufferUnmap(wb->readback);

    return HDRG_OK;
}

static void hdrg_wb_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wb_work_t *work = (hdrg_wb_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_wb_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wb_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wb_t *wb;
    hdrg_status_t status;

    wb = (hdrg_wb_t *)api->mem_calloc(api, 1, sizeof(*wb));
    if (wb == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &wb->strength);
    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_WB_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, HDRG_DEVICE_WEBGPU);
    }

    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &wb->gpu);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(wb->gpu, hdrg_wb_build, wb)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !wb->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        if (wb->gpu != NULL) {
            (void)hdrg_gpu_call(wb->gpu, hdrg_wb_release, wb);
        }

        hdrg_gpu_destroy(wb->gpu);
        api->mem_free(api, wb);

        return status;
    }

    *inst = wb;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wb_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wb_t *wb = (hdrg_wb_t *)inst;

    (void)hdrg_gpu_call(wb->gpu, hdrg_wb_release, wb);
    hdrg_gpu_destroy(wb->gpu);
    api->mem_free(api, wb);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wb_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wb_t *wb = (hdrg_wb_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &wb->width, &wb->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || wb->width == 0 || wb->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_deblock_thresholds(nego, api, wb->strength, &wb->alpha, &wb->beta, &wb->tc,
                                     &wb->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, wb->width, wb->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !wb->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wb_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wb_t *wb = (hdrg_wb_t *)inst;
    hdrg_wb_work_t *work;
    hdrg_status_t status;

    work = (hdrg_wb_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wb = wb;
    work->job = job;
    work->api = api;
    status = hdrg_webgpu_job_frames(wb->device, job, api, wb->width, wb->height, &work->frames);

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    work->params.width = wb->width;
    work->params.height = wb->height;
    work->params.in_stride = (uint32_t)(work->frames.in_stride / 4u);
    work->params.out_stride = (uint32_t)(work->frames.out_stride / 4u);
    work->params.alpha = wb->alpha;
    work->params.beta = wb->beta;
    work->params.tc = wb->tc;

    if (!hdrg_gpu_run(wb->gpu, hdrg_wb_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_deblock_register(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_LIT(HDRG_WB_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status =
        api->class_add_option_float(cls, HDRG_LIT("strength"), 1.0, 0.0, 4.0,
                                    HDRG_LIT("thresholds over the measured block step (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_wb_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_wb_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_wb_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_wb_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_device_release(cls, hdrg_webgpu_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
