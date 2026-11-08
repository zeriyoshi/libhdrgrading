/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_WD_ID        "hdrg.webgpu.defringe"
#define HDRG_WD_INFLIGHT  2u /* frames in flight: the next one's upload meets this one's */
#define HDRG_WD_BINDINGS  8u
#define HDRG_WD_PLANES    5u /* Y', B' - Y', R' - Y', the edges, the dilated edges */
#define HDRG_WD_GROUP     8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WD_SUBMITTED (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)

/*
 * hdrg.defringe (src/plugins/defringe/defringe.c) on the GPU
 * (src/plugins/defringe/wgsl/defringe.wgsl): the same windows, means and
 * passthrough, whole RGB16 frames uploaded, defringed in four dispatches and
 * read back; the output equals the CPU's. ASYNC: the jobs run in order on the
 * device's thread (src/gpu/webgpu), HDRG_WD_INFLIGHT of them in flight.
 */
typedef struct {
    hdrg_gpu_t *gpu;
    hdrg_webgpu_device_frame_t device[HDRG_WEBGPU_DEVICE_FRAMES]; /* to the next filter */
    WGPUBindGroupLayout group_layout;                             /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline planes, edges, wides, fringe;
    WGPUBuffer params, input, plane[HDRG_WD_PLANES], output, readback;
    size_t in_size, out_size, pixels; /* the buffers' (0: none yet) */
    uint32_t width, height, reach;    /* configure's */
    bool active;
    bool built; /* create: the pipelines */
    double strength;
} hdrg_wd_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride, reach, pad[3];
} hdrg_wd_params_t;

/* a job as submitted: what its run needs (the next frame's configure may come first) */
typedef struct {
    hdrg_wd_t *wd;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    hdrg_webgpu_job_t frames;
    uint32_t width, height, reach;
} hdrg_wd_work_t;

static void hdrg_wd_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

/* the device's thread: the layouts and the four pipelines */
static void hdrg_wd_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WD_BINDINGS] = {
        WGPUBufferBindingType_Uniform, WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage,
        WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage,
        WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage};
    hdrg_wd_t *wd = (hdrg_wd_t *)arg;
    WGPUBindGroupLayoutEntry entries[HDRG_WD_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;

    for (i = 0; i < HDRG_WD_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    group_desc.entryCount = HDRG_WD_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    wd->group_layout = wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &wd->group_layout;
    wd->layout = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_defringe);
    if (module == NULL) {
        return;
    }

    wd->planes = hdrg_gpu_pipeline(gpu, module, "planes", wd->layout);
    wd->edges = hdrg_gpu_pipeline(gpu, module, "edges", wd->layout);
    wd->wides = hdrg_gpu_pipeline(gpu, module, "wides", wd->layout);
    wd->fringe = hdrg_gpu_pipeline(gpu, module, "fringe", wd->layout);
    wd->params = hdrg_gpu_buffer(gpu, sizeof(hdrg_wd_params_t),
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);

    wd->built = wd->planes != NULL && wd->edges != NULL && wd->wides != NULL &&
                wd->fringe != NULL && wd->params != NULL;
}

/* the device's thread: every object */
static void hdrg_wd_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wd_t *wd = (hdrg_wd_t *)arg;
    WGPUComputePipeline *pipelines[4];
    uint8_t i;

    (void)gpu;

    pipelines[0] = &wd->planes;
    pipelines[1] = &wd->edges;
    pipelines[2] = &wd->wides;
    pipelines[3] = &wd->fringe;

    for (i = 0; i < 4u; i++) {
        if (*pipelines[i] != NULL) {
            wgpuComputePipelineRelease(*pipelines[i]);
        }
    }

    if (wd->layout != NULL) {
        wgpuPipelineLayoutRelease(wd->layout);
    }

    if (wd->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(wd->group_layout);
    }

    hdrg_wd_release_buffer(&wd->params);
    hdrg_wd_release_buffer(&wd->input);

    for (i = 0; i < HDRG_WD_PLANES; i++) {
        hdrg_wd_release_buffer(&wd->plane[i]);
    }

    hdrg_wd_release_buffer(&wd->output);
    hdrg_wd_release_buffer(&wd->readback);
    hdrg_webgpu_device_frames_free(wd->device);
}

/* the device's thread: the frame buffers for a job's sizes, kept while they are large enough */
static bool hdrg_wd_buffers(hdrg_gpu_t *gpu, hdrg_wd_t *wd, size_t in_size, size_t out_size,
                            size_t pixels)
{
    uint8_t i;
    bool ok;

    if (in_size > wd->in_size) {
        hdrg_wd_release_buffer(&wd->input);
        wd->input = hdrg_gpu_buffer(gpu, in_size, HDRG_WD_SUBMITTED);
        wd->in_size = (wd->input != NULL) ? in_size : 0u;
    }

    if (pixels > wd->pixels) {
        ok = true;

        for (i = 0; i < HDRG_WD_PLANES; i++) {
            hdrg_wd_release_buffer(&wd->plane[i]);
            wd->plane[i] = hdrg_gpu_buffer(gpu, pixels * 4u, WGPUBufferUsage_Storage);
            ok = ok && wd->plane[i] != NULL;
        }

        wd->pixels = ok ? pixels : 0u;
    }

    if (out_size > wd->out_size) {
        hdrg_wd_release_buffer(&wd->output);
        hdrg_wd_release_buffer(&wd->readback);
        wd->output =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        wd->readback =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        wd->out_size = (wd->output != NULL && wd->readback != NULL) ? out_size : 0u;
    }

    return wd->in_size >= in_size && wd->pixels >= pixels && wd->out_size >= out_size;
}

/* the device's thread: a job's upload, dispatches and read-back */
static hdrg_status_t hdrg_wd_frame(hdrg_gpu_t *gpu, const hdrg_wd_work_t *work)
{
    const size_t in_size = work->frames.in_stride * work->height,
                 out_size = work->frames.out_stride * work->height;
    const void *mapped;
    hdrg_wd_t *wd = work->wd;
    WGPUBindGroupEntry entries[HDRG_WD_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WD_BINDINGS], in_buffer, out_buffer;
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    hdrg_wd_params_t params;
    uint32_t i, groups_x, groups_y;
    bool ok;

    /* (the frames on the device stand in for the input and output buffers: none made) */
    if (!hdrg_wd_buffers(gpu, wd, (work->frames.in_frame != NULL) ? 0u : in_size,
                         (work->frames.out_frame != NULL) ? 0u : out_size,
                         (size_t)work->width * work->height)) {
        return HDRG_ERR_NOMEM;
    }

    /* the device frames: the previous filter's for the input, one of its own for the output */
    if (work->frames.out_frame != NULL &&
        !hdrg_webgpu_device_frame_fit(gpu, work->frames.out_frame, out_size)) {
        return HDRG_ERR_NOMEM;
    }

    in_buffer =
        (work->frames.in_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.in_frame) : wd->input;
    out_buffer = (work->frames.out_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.out_frame)
                                                  : wd->output;

    memset(&params, 0, sizeof(params));
    params.width = work->width;
    params.height = work->height;
    params.in_stride = (uint32_t)(work->frames.in_stride / 4u);
    params.out_stride = (uint32_t)(work->frames.out_stride / 4u);
    params.reach = work->reach;

    buffers[0] = wd->params;
    buffers[1] = in_buffer;

    for (i = 0; i < HDRG_WD_PLANES; i++) {
        buffers[2u + i] = wd->plane[i];
    }

    buffers[7] = out_buffer;

    for (i = 0; i < HDRG_WD_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = wd->group_layout;
    group_desc.entryCount = HDRG_WD_BINDINGS;
    group_desc.entries = entries;
    groups_x = (work->width + HDRG_WD_GROUP - 1u) / HDRG_WD_GROUP;
    groups_y = (work->height + HDRG_WD_GROUP - 1u) / HDRG_WD_GROUP;

    hdrg_gpu_scope_begin(gpu);

    if (work->frames.in_frame == NULL) {
        wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wd->input, 0, work->frames.in, in_size);
    }

    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wd->params, 0, &params, sizeof(params));
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderSetPipeline(pass, wd->planes);
    wgpuComputePassEncoderDispatchWorkgroups(pass, groups_x, groups_y, 1);
    wgpuComputePassEncoderSetPipeline(pass, wd->edges);
    wgpuComputePassEncoderDispatchWorkgroups(pass, groups_x, groups_y, 1);
    wgpuComputePassEncoderSetPipeline(pass, wd->wides);
    wgpuComputePassEncoderDispatchWorkgroups(pass, groups_x, groups_y, 1);
    wgpuComputePassEncoderSetPipeline(pass, wd->fringe);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, ((work->width + 1u) / 2u + HDRG_WD_GROUP - 1u) / HDRG_WD_GROUP, groups_y, 1);
    wgpuComputePassEncoderEnd(pass);

    if (work->frames.out_frame == NULL) {
        wgpuCommandEncoderCopyBufferToBuffer(encoder, wd->output, 0, wd->readback, 0, out_size);
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

    mapped = ok ? hdrg_gpu_map_read(gpu, wd->readback, out_size) : NULL;

    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->frames.out, mapped, out_size);
    wgpuBufferUnmap(wd->readback);

    return HDRG_OK;
}

static void hdrg_wd_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wd_work_t *work = (hdrg_wd_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_wd_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wd_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wd_t *wd;
    hdrg_status_t status;

    wd = (hdrg_wd_t *)api->mem_calloc(api, 1, sizeof(*wd));
    if (wd == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &wd->strength);
    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_WD_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, HDRG_DEVICE_WEBGPU);
    }

    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &wd->gpu);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(wd->gpu, hdrg_wd_build, wd)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !wd->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        if (wd->gpu != NULL) {
            (void)hdrg_gpu_call(wd->gpu, hdrg_wd_release, wd);
        }

        hdrg_gpu_destroy(wd->gpu);
        api->mem_free(api, wd);

        return status;
    }

    *inst = wd;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wd_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wd_t *wd = (hdrg_wd_t *)inst;

    (void)hdrg_gpu_call(wd->gpu, hdrg_wd_release, wd);
    hdrg_gpu_destroy(wd->gpu);
    api->mem_free(api, wd);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wd_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wd_t *wd = (hdrg_wd_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &wd->width, &wd->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || wd->width == 0 || wd->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_defringe_settings(nego, api, wd->strength, &wd->reach, &wd->active);
    if (status != HDRG_OK) {
        return status;
    }

    status = api->nego_set_output(nego, wd->width, wd->height, HDRG_FORMAT_RGB16);

    return (status == HDRG_OK && !wd->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wd_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wd_t *wd = (hdrg_wd_t *)inst;
    hdrg_wd_work_t *work;
    hdrg_status_t status;

    work = (hdrg_wd_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wd = wd;
    work->job = job;
    work->api = api;
    work->width = wd->width;
    work->height = wd->height;
    work->reach = wd->reach;
    status = hdrg_webgpu_job_frames(wd->device, job, api, wd->width, wd->height, &work->frames);

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    if (!hdrg_gpu_run(wd->gpu, hdrg_wd_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_defringe_register(hdrg_plugin_t *plugin,
                                                   const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_LIT(HDRG_WD_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_LIT("strength"), 3.0, 0.0, 3.0,
        HDRG_LIT("sets the window's radius, 1 + 2 x strength (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_wd_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_wd_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_wd_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_wd_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_device_release(cls, hdrg_webgpu_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
