/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * hdrg.webgpu.nis ports NVIDIA Image Scaling 1.0.3 (MIT; its notice is in
 * nis.h).
 */

#include <string.h>

#include "nis.h"
#include "parts.h"

#define HDRG_WN_ID        "hdrg.webgpu.nis"
#define HDRG_WN_LIT(s)    s, sizeof(s) - 1u
#define HDRG_WN_INFLIGHT  2u /* frames in flight: the next one's upload meets this one's */
#define HDRG_WN_BINDINGS  5u
#define HDRG_WN_GROUP     8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WN_SUBMITTED (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)
#define HDRG_WN_COPY      1u /* Params.flags: the source's codes (the same size) */

/*
 * hdrg.scale.nis (src/plugins/nis/nis.c) on WebGPU: src/plugins/nis/wgsl/nis.wgsl
 * over a whole RGB16 frame uploaded, its luma then the output, read back,
 * within the CPU's output (the GPU's divisions may round otherwise). ASYNC,
 * HDRG_WN_INFLIGHT frames in flight; the device's thread runs the jobs one
 * at a time over the filter's buffers; the tables uploaded once.
 */
typedef struct {
    hdrg_gpu_t *gpu;
    WGPUBindGroupLayout group_layout; /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline luma, scale;
    WGPUBuffer params, input, coefs, lumas, output, readback;
    hdrg_nis_config_t config;            /* configure's */
    size_t in_size, luma_size, out_size; /* the buffers' (0: none yet) */
    uint32_t in_w, in_h, out_w, out_h;   /* configure's */
    bool built;                          /* create's pipelines and tables */
    float sharpness;
} hdrg_wn_t;

/* the shader's uniform (Params, padded to 96 bytes) */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h, in_stride, out_stride, flags, pad;
    float scale_x, scale_y, detect_ratio, detect_thres, min_contrast_ratio, ratio_norm,
        contrast_boost, eps, sharp_start_y, sharp_scale_y, sharp_strength_min, sharp_strength_scale,
        sharp_limit_min, sharp_limit_scale, unused[2];
} hdrg_wn_params_t;

typedef struct {
    hdrg_wn_t *wn;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    const uint8_t *in;
    uint8_t *out;
    size_t in_stride, out_stride;
    hdrg_wn_params_t params;
} hdrg_wn_work_t;

static void hdrg_wn_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

static void hdrg_wn_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WN_BINDINGS] = {
        WGPUBufferBindingType_Uniform, WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Storage,
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

    module = hdrg_gpu_module(gpu, hdrg_wgsl_nis);
    if (module == NULL) {
        return;
    }

    wn->luma = hdrg_gpu_pipeline(gpu, module, "luma", wn->layout);
    wn->scale = hdrg_gpu_pipeline(gpu, module, "scale", wn->layout);
    wn->params = hdrg_gpu_buffer(gpu, sizeof(hdrg_wn_params_t),
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wn->coefs = hdrg_gpu_buffer(gpu, 2u * sizeof(hdrg_nis_coef_scale), HDRG_WN_SUBMITTED);
    wgpuShaderModuleRelease(module);

    /* the tables: coef_scale, then coef_usm */
    if (wn->coefs != NULL) {
        wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wn->coefs, 0, hdrg_nis_coef_scale,
                             sizeof(hdrg_nis_coef_scale));
        wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wn->coefs, sizeof(hdrg_nis_coef_scale),
                             hdrg_nis_coef_usm, sizeof(hdrg_nis_coef_usm));
    }

    wn->built = wn->luma != NULL && wn->scale != NULL && wn->params != NULL && wn->coefs != NULL;
}

static void hdrg_wn_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wn_t *wn = (hdrg_wn_t *)arg;

    (void)gpu;

    if (wn->luma != NULL) {
        wgpuComputePipelineRelease(wn->luma);
    }

    if (wn->scale != NULL) {
        wgpuComputePipelineRelease(wn->scale);
    }

    if (wn->layout != NULL) {
        wgpuPipelineLayoutRelease(wn->layout);
    }

    if (wn->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(wn->group_layout);
    }

    hdrg_wn_release_buffer(&wn->params);
    hdrg_wn_release_buffer(&wn->input);
    hdrg_wn_release_buffer(&wn->coefs);
    hdrg_wn_release_buffer(&wn->lumas);
    hdrg_wn_release_buffer(&wn->output);
    hdrg_wn_release_buffer(&wn->readback);
}

/* the device's thread: the frame buffers for a job's sizes, kept while they are large enough */
static bool hdrg_wn_buffers(hdrg_gpu_t *gpu, hdrg_wn_t *wn, size_t in_size, size_t luma_size,
                            size_t out_size)
{
    if (in_size > wn->in_size) {
        hdrg_wn_release_buffer(&wn->input);
        wn->input = hdrg_gpu_buffer(gpu, in_size, HDRG_WN_SUBMITTED);
        wn->in_size = (wn->input != NULL) ? in_size : 0u;
    }

    if (luma_size > wn->luma_size) {
        hdrg_wn_release_buffer(&wn->lumas);
        wn->lumas = hdrg_gpu_buffer(gpu, luma_size, WGPUBufferUsage_Storage);
        wn->luma_size = (wn->lumas != NULL) ? luma_size : 0u;
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

    return wn->in_size >= in_size && wn->luma_size >= luma_size && wn->out_size >= out_size;
}

/* the device's thread: a job's upload, dispatches and read-back */
static hdrg_status_t hdrg_wn_frame(hdrg_gpu_t *gpu, const hdrg_wn_work_t *work)
{
    const hdrg_wn_params_t *params = &work->params;
    const size_t in_size = work->in_stride * params->in_h,
                 luma_size = (size_t)params->in_w * params->in_h * sizeof(float),
                 out_size = work->out_stride * params->out_h;
    const void *mapped;
    hdrg_wn_t *wn = work->wn;
    WGPUBindGroupEntry entries[HDRG_WN_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WN_BINDINGS];
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    uint32_t i;
    bool ok;

    if (!hdrg_wn_buffers(gpu, wn, in_size, luma_size, out_size)) {
        return HDRG_ERR_NOMEM;
    }

    buffers[0] = wn->params;
    buffers[1] = wn->input;
    buffers[2] = wn->coefs;
    buffers[3] = wn->lumas;
    buffers[4] = wn->output;

    for (i = 0; i < HDRG_WN_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = wn->group_layout;
    group_desc.entryCount = HDRG_WN_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wn->params, 0, params, sizeof(*params));
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wn->input, 0, work->in, in_size);
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderSetPipeline(pass, wn->luma);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (params->in_w + HDRG_WN_GROUP - 1u) / HDRG_WN_GROUP,
        (params->in_h + HDRG_WN_GROUP - 1u) / HDRG_WN_GROUP, 1);
    wgpuComputePassEncoderSetPipeline(pass, wn->scale);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, ((params->out_w + 1u) / 2u + HDRG_WN_GROUP - 1u) / HDRG_WN_GROUP,
        (params->out_h + HDRG_WN_GROUP - 1u) / HDRG_WN_GROUP, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, wn->output, 0, wn->readback, 0, out_size);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(group);
    ok = hdrg_gpu_scope_end(gpu);

    mapped = ok ? hdrg_gpu_map_read(gpu, wn->readback, out_size) : NULL;
    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->out, mapped, out_size);
    wgpuBufferUnmap(wn->readback);

    return HDRG_OK;
}

static void hdrg_wn_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wn_work_t *work = (hdrg_wn_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_wn_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static void hdrg_wn_free(hdrg_wn_t *wn, const hdrg_plugin_api_t *api)
{
    if (wn->gpu != NULL) {
        (void)hdrg_gpu_call(wn->gpu, hdrg_wn_release, wn);
    }

    hdrg_gpu_destroy(wn->gpu);
    api->mem_free(api, wn);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wn_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wn_t *wn;
    hdrg_status_t status;
    double sharpness;

    wn = (hdrg_wn_t *)api->mem_calloc(api, 1, sizeof(*wn));
    if (wn == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_WN_LIT("sharpness"), &sharpness);
    if (status == HDRG_OK) {
        wn->sharpness = (float)sharpness;
        status = api->filter_set_max_inflight(filter, HDRG_WN_INFLIGHT);
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
        hdrg_wn_free(wn, api);

        return status;
    }

    *inst = wn;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wn_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wn_free((hdrg_wn_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wn_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wn_t *wn = (hdrg_wn_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &wn->in_w, &wn->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &wn->out_w, &wn->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || wn->in_w == 0 || wn->in_h == 0 || wn->out_w == 0 ||
        wn->out_h == 0 ||
        !hdrg_nis_setup(wn->sharpness, wn->in_w, wn->in_h, wn->out_w, wn->out_h, &wn->config)) {
        return HDRG_ERR_UNSUPPORTED;
    }

    return api->nego_set_output(nego, wn->out_w, wn->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wn_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const hdrg_nis_config_t *c = &((hdrg_wn_t *)inst)->config;
    hdrg_wn_t *wn = (hdrg_wn_t *)inst;
    hdrg_wn_work_t *work;
    hdrg_wn_params_t *params;
    hdrg_status_t status;
    uint32_t begin, end, row_begin, row_end;

    work = (hdrg_wn_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wn = wn;
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
        (begin != 0u || end != wn->out_h || row_begin != 0u || row_end != wn->in_h ||
         work->in_stride % 4u != 0u || work->out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    params = &work->params;
    params->in_w = wn->in_w;
    params->in_h = wn->in_h;
    params->out_w = wn->out_w;
    params->out_h = wn->out_h;
    params->in_stride = (uint32_t)(work->in_stride / 4u);
    params->out_stride = (uint32_t)(work->out_stride / 4u);
    params->flags = (wn->out_w == wn->in_w && wn->out_h == wn->in_h) ? HDRG_WN_COPY : 0u;
    params->scale_x = c->scale_x;
    params->scale_y = c->scale_y;
    params->detect_ratio = c->detect_ratio;
    params->detect_thres = c->detect_thres;
    params->min_contrast_ratio = c->min_contrast_ratio;
    params->ratio_norm = c->ratio_norm;
    params->contrast_boost = c->contrast_boost;
    params->eps = c->eps;
    params->sharp_start_y = c->sharp_start_y;
    params->sharp_scale_y = c->sharp_scale_y;
    params->sharp_strength_min = c->sharp_strength_min;
    params->sharp_strength_scale = c->sharp_strength_scale;
    params->sharp_limit_min = c->sharp_limit_min;
    params->sharp_limit_scale = c->sharp_limit_scale;

    if (!hdrg_gpu_run(wn->gpu, hdrg_wn_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_nis_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_WN_LIT(HDRG_WN_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(cls, HDRG_WN_LIT("sharpness"), 0.5, 0.0, 1.0,
                                         HDRG_WN_LIT("NIS's sharpness (0 = none, 1 = the most)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
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
        status = api->class_register(cls);
    }

    return status;
}
