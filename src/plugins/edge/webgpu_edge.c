/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "edge.h"
#include "parts.h"

#define HDRG_WE_ID        "hdrg.webgpu.edge"
#define HDRG_WE_LIT(s)    s, sizeof(s) - 1u
#define HDRG_WE_INFLIGHT  2u /* frames in flight: the next one's upload meets this one's */
#define HDRG_WE_BINDINGS  4u
#define HDRG_WE_GROUP     8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WE_SUBMITTED (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)
#define HDRG_WE_COPY      1u /* Params.flags: the source's codes (the same size) */

/*
 * hdrg.scale.edge (src/plugins/edge/edge.c) on WebGPU:
 * src/plugins/edge/wgsl/edge.wgsl over a whole RGB16 frame uploaded, its luma
 * then the output, read back, within the CPU's output (the GPU's divisions
 * may round otherwise). ASYNC, HDRG_WE_INFLIGHT frames in flight; the
 * device's thread runs the jobs one at a time over the filter's buffers
 */
typedef struct {
    hdrg_gpu_t *gpu;
    WGPUBindGroupLayout group_layout; /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline luma, scale;
    WGPUBuffer params, input, lumas, output, readback;
    hdrg_edge_config_t config;           /* configure's */
    size_t in_size, luma_size, out_size; /* the buffers' (0: none yet) */
    uint32_t in_w, in_h, out_w, out_h;   /* configure's */
    bool built;                          /* create's pipelines */
} hdrg_we_t;

/* the shader's uniform (Params, 48 bytes) */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h, in_stride, out_stride, flags, pad;
    float scale_x, scale_y, stretch, sharpen;
} hdrg_we_params_t;

typedef struct {
    hdrg_we_t *we;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    const uint8_t *in;
    uint8_t *out;
    size_t in_stride, out_stride;
    hdrg_we_params_t params;
} hdrg_we_work_t;

static void hdrg_we_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

static void hdrg_we_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WE_BINDINGS] = {
        WGPUBufferBindingType_Uniform, WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage};
    hdrg_we_t *we = (hdrg_we_t *)arg;
    WGPUBindGroupLayoutEntry entries[HDRG_WE_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;

    for (i = 0; i < HDRG_WE_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    group_desc.entryCount = HDRG_WE_BINDINGS;
    group_desc.entries = entries;
    hdrg_gpu_scope_begin(gpu);
    we->group_layout = wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &we->group_layout;
    we->layout = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_edge);
    if (module == NULL) {
        return;
    }

    we->luma = hdrg_gpu_pipeline(gpu, module, "luma", we->layout);
    we->scale = hdrg_gpu_pipeline(gpu, module, "scale", we->layout);
    we->params = hdrg_gpu_buffer(gpu, sizeof(hdrg_we_params_t),
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);
    we->built = we->luma != NULL && we->scale != NULL && we->params != NULL;
}

static void hdrg_we_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_we_t *we = (hdrg_we_t *)arg;

    (void)gpu;

    if (we->luma != NULL) {
        wgpuComputePipelineRelease(we->luma);
    }

    if (we->scale != NULL) {
        wgpuComputePipelineRelease(we->scale);
    }

    if (we->layout != NULL) {
        wgpuPipelineLayoutRelease(we->layout);
    }

    if (we->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(we->group_layout);
    }

    hdrg_we_release_buffer(&we->params);
    hdrg_we_release_buffer(&we->input);
    hdrg_we_release_buffer(&we->lumas);
    hdrg_we_release_buffer(&we->output);
    hdrg_we_release_buffer(&we->readback);
}

/* the device's thread: the frame buffers for a job's sizes, kept while they are large enough */
static bool hdrg_we_buffers(hdrg_gpu_t *gpu, hdrg_we_t *we, size_t in_size, size_t luma_size,
                            size_t out_size)
{
    if (in_size > we->in_size) {
        hdrg_we_release_buffer(&we->input);
        we->input = hdrg_gpu_buffer(gpu, in_size, HDRG_WE_SUBMITTED);
        we->in_size = (we->input != NULL) ? in_size : 0u;
    }

    if (luma_size > we->luma_size) {
        hdrg_we_release_buffer(&we->lumas);
        we->lumas = hdrg_gpu_buffer(gpu, luma_size, WGPUBufferUsage_Storage);
        we->luma_size = (we->lumas != NULL) ? luma_size : 0u;
    }

    if (out_size > we->out_size) {
        hdrg_we_release_buffer(&we->output);
        hdrg_we_release_buffer(&we->readback);
        we->output =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        we->readback =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        we->out_size = (we->output != NULL && we->readback != NULL) ? out_size : 0u;
    }

    return we->in_size >= in_size && we->luma_size >= luma_size && we->out_size >= out_size;
}

/* the device's thread: a job's upload, dispatches and read-back */
static hdrg_status_t hdrg_we_frame(hdrg_gpu_t *gpu, const hdrg_we_work_t *work)
{
    const hdrg_we_params_t *params = &work->params;
    const size_t in_size = work->in_stride * params->in_h,
                 luma_size = (size_t)params->in_w * params->in_h * sizeof(float),
                 out_size = work->out_stride * params->out_h;
    const void *mapped;
    hdrg_we_t *we = work->we;
    WGPUBindGroupEntry entries[HDRG_WE_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WE_BINDINGS];
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    uint32_t i;
    bool ok;

    if (!hdrg_we_buffers(gpu, we, in_size, luma_size, out_size)) {
        return HDRG_ERR_NOMEM;
    }

    buffers[0] = we->params;
    buffers[1] = we->input;
    buffers[2] = we->lumas;
    buffers[3] = we->output;

    for (i = 0; i < HDRG_WE_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = we->group_layout;
    group_desc.entryCount = HDRG_WE_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), we->params, 0, params, sizeof(*params));
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), we->input, 0, work->in, in_size);
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderSetPipeline(pass, we->luma);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (params->in_w + HDRG_WE_GROUP - 1u) / HDRG_WE_GROUP,
        (params->in_h + HDRG_WE_GROUP - 1u) / HDRG_WE_GROUP, 1);
    wgpuComputePassEncoderSetPipeline(pass, we->scale);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, ((params->out_w + 1u) / 2u + HDRG_WE_GROUP - 1u) / HDRG_WE_GROUP,
        (params->out_h + HDRG_WE_GROUP - 1u) / HDRG_WE_GROUP, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, we->output, 0, we->readback, 0, out_size);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(group);
    ok = hdrg_gpu_scope_end(gpu);

    mapped = ok ? hdrg_gpu_map_read(gpu, we->readback, out_size) : NULL;
    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->out, mapped, out_size);
    wgpuBufferUnmap(we->readback);

    return HDRG_OK;
}

static void hdrg_we_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_we_work_t *work = (hdrg_we_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_we_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static void hdrg_we_free(hdrg_we_t *we, const hdrg_plugin_api_t *api)
{
    if (we->gpu != NULL) {
        (void)hdrg_gpu_call(we->gpu, hdrg_we_release, we);
    }

    hdrg_gpu_destroy(we->gpu);
    api->mem_free(api, we);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_we_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_we_t *we;
    hdrg_status_t status;
    double stretch, sharpen;

    we = (hdrg_we_t *)api->mem_calloc(api, 1, sizeof(*we));
    if (we == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_WE_LIT("stretch"), &stretch);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_WE_LIT("sharpen"), &sharpen);
    }

    if (status == HDRG_OK) {
        we->config.stretch = (float)stretch;
        we->config.sharpen = (float)sharpen;
        status = api->filter_set_max_inflight(filter, HDRG_WE_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &we->gpu);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(we->gpu, hdrg_we_build, we)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !we->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_we_free(we, api);

        return status;
    }

    *inst = we;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_we_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_we_free((hdrg_we_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_we_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_we_t *we = (hdrg_we_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &we->in_w, &we->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &we->out_w, &we->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || we->in_w == 0 || we->in_h == 0 || we->out_w < we->in_w ||
        we->out_h < we->in_h) {
        return HDRG_ERR_UNSUPPORTED;
    }

    we->config.scale_x = (float)we->in_w / (float)we->out_w;
    we->config.scale_y = (float)we->in_h / (float)we->out_h;

    return api->nego_set_output(nego, we->out_w, we->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_we_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_we_t *we = (hdrg_we_t *)inst;
    hdrg_we_work_t *work;
    hdrg_we_params_t *params;
    hdrg_status_t status;
    uint32_t begin, end, row_begin, row_end;

    work = (hdrg_we_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->we = we;
    work->job = job;
    work->api = api;
    status = api->job_rows(job, &begin, &end);

    if (status == HDRG_OK) {
        status = api->job_input(job, 0, &work->in, &work->in_stride, &row_begin, &row_end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &work->out, &work->out_stride);
    }

    if (status == HDRG_OK &&
        (begin != 0u || end != we->out_h || row_begin != 0u || row_end != we->in_h ||
         work->in_stride % 4u != 0u || work->out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    params = &work->params;
    params->in_w = we->in_w;
    params->in_h = we->in_h;
    params->out_w = we->out_w;
    params->out_h = we->out_h;
    params->in_stride = (uint32_t)(work->in_stride / 4u);
    params->out_stride = (uint32_t)(work->out_stride / 4u);
    params->flags = (we->out_w == we->in_w && we->out_h == we->in_h) ? HDRG_WE_COPY : 0u;
    params->scale_x = we->config.scale_x;
    params->scale_y = we->config.scale_y;
    params->stretch = we->config.stretch;
    params->sharpen = we->config.sharpen;

    if (!hdrg_gpu_run(we->gpu, hdrg_we_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_edge_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_WE_LIT(HDRG_WE_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_WE_LIT("stretch"), HDRG_EDGE_STRETCH, 0.0, 3.0,
        HDRG_WE_LIT("the kernel's widening along an edge, by its coherence (0 = none)"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_WE_LIT("sharpen"), HDRG_EDGE_SHARPEN, 0.0, 2.0,
            HDRG_WE_LIT("the kernel's narrowing across an edge, by its coherence (0 = none)"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_we_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_we_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_we_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_we_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
