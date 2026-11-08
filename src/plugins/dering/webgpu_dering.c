/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_WR_ID        "hdrg.webgpu.dering"
#define HDRG_WR_INFLIGHT  2u /* frames in flight: the next one's upload meets this one's */
#define HDRG_WR_BINDINGS  4u
#define HDRG_WR_GROUP     8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WR_GRID      8u /* the blocks' grid */
#define HDRG_WR_SUBMITTED (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)

/*
 * hdrg.dering (src/plugins/dering/dering.c) on the GPU
 * (src/plugins/dering/wgsl/dering.wgsl): the same settings from the frame's
 * noise, blocks, ranges, windows and passthrough, whole RGB16 frames
 * uploaded, deringed in two dispatches and read back; the output equals the
 * CPU's. ASYNC: the jobs run in order on the device's thread (src/gpu/webgpu),
 * HDRG_WR_INFLIGHT of them in flight.
 */
typedef struct {
    hdrg_gpu_t *gpu;
    hdrg_webgpu_device_frame_t device[HDRG_WEBGPU_DEVICE_FRAMES]; /* to the next filter */
    WGPUBindGroupLayout group_layout;                             /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline contrasts, dering;
    WGPUBuffer params, input, contrast, output, readback;
    size_t in_size, blocks_size, out_size; /* the buffers' (0: none yet) */
    uint32_t width, height, reach;         /* configure's */
    int32_t floor;                         /* configure's: hdrg.dering's settings */
    bool active;
    bool built; /* create: the pipelines */
    double strength;
} hdrg_wr_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride, blocks, reach;
    int32_t floor;
    uint32_t pad;
} hdrg_wr_params_t;

/* a job as submitted: what its run needs (the next frame's configure may come first) */
typedef struct {
    hdrg_wr_t *wr;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    hdrg_webgpu_job_t frames;
    hdrg_wr_params_t params;
} hdrg_wr_work_t;

static void hdrg_wr_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

/* the device's thread: the layouts and the two pipelines */
static void hdrg_wr_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WR_BINDINGS] = {
        WGPUBufferBindingType_Uniform, WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage};
    hdrg_wr_t *wr = (hdrg_wr_t *)arg;
    WGPUBindGroupLayoutEntry entries[HDRG_WR_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;

    for (i = 0; i < HDRG_WR_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    group_desc.entryCount = HDRG_WR_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    wr->group_layout = wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &wr->group_layout;
    wr->layout = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_dering);
    if (module == NULL) {
        return;
    }

    wr->contrasts = hdrg_gpu_pipeline(gpu, module, "contrasts", wr->layout);
    wr->dering = hdrg_gpu_pipeline(gpu, module, "dering", wr->layout);
    wr->params = hdrg_gpu_buffer(gpu, sizeof(hdrg_wr_params_t),
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);

    wr->built = wr->contrasts != NULL && wr->dering != NULL && wr->params != NULL;
}

/* the device's thread: every object */
static void hdrg_wr_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wr_t *wr = (hdrg_wr_t *)arg;

    (void)gpu;

    if (wr->contrasts != NULL) {
        wgpuComputePipelineRelease(wr->contrasts);
    }

    if (wr->dering != NULL) {
        wgpuComputePipelineRelease(wr->dering);
    }

    if (wr->layout != NULL) {
        wgpuPipelineLayoutRelease(wr->layout);
    }

    if (wr->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(wr->group_layout);
    }

    hdrg_wr_release_buffer(&wr->params);
    hdrg_wr_release_buffer(&wr->input);
    hdrg_wr_release_buffer(&wr->contrast);
    hdrg_wr_release_buffer(&wr->output);
    hdrg_wr_release_buffer(&wr->readback);
    hdrg_webgpu_device_frames_free(wr->device);
}

/* the device's thread: the frame and block buffers for a job's sizes, kept while large enough */
static bool hdrg_wr_buffers(hdrg_gpu_t *gpu, hdrg_wr_t *wr, size_t in_size, size_t blocks_size,
                            size_t out_size)
{
    if (in_size > wr->in_size) {
        hdrg_wr_release_buffer(&wr->input);
        wr->input = hdrg_gpu_buffer(gpu, in_size, HDRG_WR_SUBMITTED);
        wr->in_size = (wr->input != NULL) ? in_size : 0u;
    }

    if (blocks_size > wr->blocks_size) {
        hdrg_wr_release_buffer(&wr->contrast);
        wr->contrast = hdrg_gpu_buffer(gpu, blocks_size, WGPUBufferUsage_Storage);
        wr->blocks_size = (wr->contrast != NULL) ? blocks_size : 0u;
    }

    if (out_size > wr->out_size) {
        hdrg_wr_release_buffer(&wr->output);
        hdrg_wr_release_buffer(&wr->readback);
        wr->output =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        wr->readback =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        wr->out_size = (wr->output != NULL && wr->readback != NULL) ? out_size : 0u;
    }

    return wr->in_size >= in_size && wr->blocks_size >= blocks_size && wr->out_size >= out_size;
}

/* the device's thread: a job's upload, dispatches and read-back */
static hdrg_status_t hdrg_wr_frame(hdrg_gpu_t *gpu, const hdrg_wr_work_t *work)
{
    const size_t in_size = work->frames.in_stride * work->params.height,
                 out_size = work->frames.out_stride * work->params.height;
    const void *mapped;
    hdrg_wr_t *wr = work->wr;
    WGPUBindGroupEntry entries[HDRG_WR_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WR_BINDINGS], in_buffer, out_buffer;
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    uint32_t i, rows;
    size_t blocks_size;
    bool ok;

    rows = (work->params.height + HDRG_WR_GRID - 1u) / HDRG_WR_GRID;
    blocks_size = (size_t)work->params.blocks * rows * sizeof(int32_t);

    /* (the frames on the device stand in for the input and output buffers: none made) */
    if (!hdrg_wr_buffers(gpu, wr, (work->frames.in_frame != NULL) ? 0u : in_size, blocks_size,
                         (work->frames.out_frame != NULL) ? 0u : out_size)) {
        return HDRG_ERR_NOMEM;
    }

    /* the device frames: the previous filter's for the input, one of its own for the output */
    if (work->frames.out_frame != NULL &&
        !hdrg_webgpu_device_frame_fit(gpu, work->frames.out_frame, out_size)) {
        return HDRG_ERR_NOMEM;
    }

    in_buffer =
        (work->frames.in_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.in_frame) : wr->input;
    out_buffer = (work->frames.out_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.out_frame)
                                                  : wr->output;

    buffers[0] = wr->params;
    buffers[1] = in_buffer;
    buffers[2] = wr->contrast;
    buffers[3] = out_buffer;

    for (i = 0; i < HDRG_WR_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = wr->group_layout;
    group_desc.entryCount = HDRG_WR_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    if (work->frames.in_frame == NULL) {
        wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wr->input, 0, work->frames.in, in_size);
    }

    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wr->params, 0, &work->params, sizeof(work->params));
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderSetPipeline(pass, wr->contrasts);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (work->params.blocks + HDRG_WR_GROUP - 1u) / HDRG_WR_GROUP,
        (rows + HDRG_WR_GROUP - 1u) / HDRG_WR_GROUP, 1);
    wgpuComputePassEncoderSetPipeline(pass, wr->dering);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, ((work->params.width + 1u) / 2u + HDRG_WR_GROUP - 1u) / HDRG_WR_GROUP,
        (work->params.height + HDRG_WR_GROUP - 1u) / HDRG_WR_GROUP, 1);
    wgpuComputePassEncoderEnd(pass);

    if (work->frames.out_frame == NULL) {
        wgpuCommandEncoderCopyBufferToBuffer(encoder, wr->output, 0, wr->readback, 0, out_size);
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

    mapped = ok ? hdrg_gpu_map_read(gpu, wr->readback, out_size) : NULL;

    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->frames.out, mapped, out_size);
    wgpuBufferUnmap(wr->readback);

    return HDRG_OK;
}

static void hdrg_wr_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wr_work_t *work = (hdrg_wr_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_wr_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wr_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wr_t *wr;
    hdrg_status_t status;

    wr = (hdrg_wr_t *)api->mem_calloc(api, 1, sizeof(*wr));
    if (wr == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &wr->strength);
    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_WR_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, HDRG_DEVICE_WEBGPU);
    }

    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &wr->gpu);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(wr->gpu, hdrg_wr_build, wr)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !wr->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        if (wr->gpu != NULL) {
            (void)hdrg_gpu_call(wr->gpu, hdrg_wr_release, wr);
        }

        hdrg_gpu_destroy(wr->gpu);
        api->mem_free(api, wr);

        return status;
    }

    *inst = wr;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wr_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wr_t *wr = (hdrg_wr_t *)inst;

    (void)hdrg_gpu_call(wr->gpu, hdrg_wr_release, wr);
    hdrg_gpu_destroy(wr->gpu);
    api->mem_free(api, wr);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wr_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wr_t *wr = (hdrg_wr_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &wr->width, &wr->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || wr->width == 0 || wr->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_dering_settings(nego, api, wr->strength, &wr->reach, &wr->floor, &wr->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, wr->width, wr->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !wr->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wr_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wr_t *wr = (hdrg_wr_t *)inst;
    hdrg_wr_work_t *work;
    hdrg_status_t status;

    work = (hdrg_wr_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wr = wr;
    work->job = job;
    work->api = api;
    status = hdrg_webgpu_job_frames(wr->device, job, api, wr->width, wr->height, &work->frames);

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    work->params.width = wr->width;
    work->params.height = wr->height;
    work->params.in_stride = (uint32_t)(work->frames.in_stride / 4u);
    work->params.out_stride = (uint32_t)(work->frames.out_stride / 4u);
    work->params.blocks = (wr->width + HDRG_WR_GRID - 1u) / HDRG_WR_GRID;
    work->params.reach = wr->reach;
    work->params.floor = wr->floor;

    if (!hdrg_gpu_run(wr->gpu, hdrg_wr_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_dering_register(hdrg_plugin_t *plugin,
                                                 const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_LIT(HDRG_WR_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_LIT("strength"), 1.0, 0.0, 3.0,
        HDRG_LIT("sets the window's radius, 1 + 2 x strength (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_wr_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_wr_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_wr_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_wr_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_device_release(cls, hdrg_webgpu_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
