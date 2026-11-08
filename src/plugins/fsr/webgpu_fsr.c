/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * hdrg.webgpu.fsr1 ports AMD FidelityFX Super Resolution 1.0.2 (MIT; its
 * notice is in fsr.h).
 */

#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "fsr.h"
#include "parts.h"

#define HDRG_WF_ID        "hdrg.webgpu.fsr1"
#define HDRG_WF_LIT(s)    s, sizeof(s) - 1u
#define HDRG_WF_INFLIGHT  2u /* frames in flight: the next one's upload meets this one's */
#define HDRG_WF_BINDINGS  4u
#define HDRG_WF_GROUP     8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WF_SUBMITTED (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)
#define HDRG_WF_MID       1u /* Params.flags: EASU into mid (RCAS follows) */
#define HDRG_WF_COPY      2u /* ... the source's codes (the same size) */

/*
 * hdrg.scale.fsr1 (src/plugins/fsr/fsr.c) on WebGPU: src/plugins/fsr/wgsl/fsr.wgsl
 * over a whole RGB16 frame uploaded, EASU (and with sharpen RCAS over its
 * floats) and read back, within the CPU's output (the GPU's divisions may
 * round otherwise). ASYNC, HDRG_WF_INFLIGHT frames in flight; the device's
 * thread runs the jobs one at a time over the filter's buffers.
 */
typedef struct {
    hdrg_gpu_t *gpu;
    WGPUBindGroupLayout group_layout; /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline easu, rcas;
    WGPUBuffer params, input, mid, output, readback;
    size_t in_size, mid_size, out_size; /* the buffers' (0: none yet) */
    uint32_t in_w, in_h, out_w, out_h;  /* configure's */
    bool sharpen, built;                /* built: create's pipelines */
    float scale[2], offset[2], sharpness;
} hdrg_wf_t;

/* the shader's uniform (Params, padded to 64 bytes) */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h, in_stride, out_stride, flags, pad;
    float scale_x, scale_y, offset_x, offset_y, sharpness, unused[3];
} hdrg_wf_params_t;

typedef struct {
    hdrg_wf_t *wf;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    const uint8_t *in;
    uint8_t *out;
    size_t in_stride, out_stride;
    hdrg_wf_params_t params;
} hdrg_wf_work_t;

static void hdrg_wf_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

static void hdrg_wf_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WF_BINDINGS] = {
        WGPUBufferBindingType_Uniform, WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage};
    hdrg_wf_t *wf = (hdrg_wf_t *)arg;
    WGPUBindGroupLayoutEntry entries[HDRG_WF_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;

    for (i = 0; i < HDRG_WF_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    group_desc.entryCount = HDRG_WF_BINDINGS;
    group_desc.entries = entries;
    hdrg_gpu_scope_begin(gpu);
    wf->group_layout = wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &wf->group_layout;
    wf->layout = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_fsr);
    if (module == NULL) {
        return;
    }

    wf->easu = hdrg_gpu_pipeline(gpu, module, "easu", wf->layout);
    wf->rcas = hdrg_gpu_pipeline(gpu, module, "rcas", wf->layout);
    wf->params = hdrg_gpu_buffer(gpu, sizeof(hdrg_wf_params_t),
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);
    wf->built = wf->easu != NULL && wf->rcas != NULL && wf->params != NULL;
}

static void hdrg_wf_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wf_t *wf = (hdrg_wf_t *)arg;

    (void)gpu;

    if (wf->easu != NULL) {
        wgpuComputePipelineRelease(wf->easu);
    }

    if (wf->rcas != NULL) {
        wgpuComputePipelineRelease(wf->rcas);
    }

    if (wf->layout != NULL) {
        wgpuPipelineLayoutRelease(wf->layout);
    }

    if (wf->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(wf->group_layout);
    }

    hdrg_wf_release_buffer(&wf->params);
    hdrg_wf_release_buffer(&wf->input);
    hdrg_wf_release_buffer(&wf->mid);
    hdrg_wf_release_buffer(&wf->output);
    hdrg_wf_release_buffer(&wf->readback);
}

/* the device's thread: the frame buffers for a job's sizes, kept while they are large enough */
static bool hdrg_wf_buffers(hdrg_gpu_t *gpu, hdrg_wf_t *wf, size_t in_size, size_t mid_size,
                            size_t out_size)
{
    if (in_size > wf->in_size) {
        hdrg_wf_release_buffer(&wf->input);
        wf->input = hdrg_gpu_buffer(gpu, in_size, HDRG_WF_SUBMITTED);
        wf->in_size = (wf->input != NULL) ? in_size : 0u;
    }

    if (mid_size > wf->mid_size) {
        hdrg_wf_release_buffer(&wf->mid);
        wf->mid = hdrg_gpu_buffer(gpu, mid_size, WGPUBufferUsage_Storage);
        wf->mid_size = (wf->mid != NULL) ? mid_size : 0u;
    }

    if (out_size > wf->out_size) {
        hdrg_wf_release_buffer(&wf->output);
        hdrg_wf_release_buffer(&wf->readback);
        wf->output =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        wf->readback =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        wf->out_size = (wf->output != NULL && wf->readback != NULL) ? out_size : 0u;
    }

    return wf->in_size >= in_size && wf->mid_size >= mid_size && wf->out_size >= out_size;
}

/* the device's thread: a job's upload, dispatches and read-back */
static hdrg_status_t hdrg_wf_frame(hdrg_gpu_t *gpu, const hdrg_wf_work_t *work)
{
    const hdrg_wf_params_t *params = &work->params;
    const size_t in_size = work->in_stride * params->in_h,
                 mid_size = ((params->flags & HDRG_WF_MID) != 0u)
                                ? (size_t)params->out_w * params->out_h * 3u * sizeof(float)
                                : 16u,
                 out_size = work->out_stride * params->out_h;
    const bool sharpen = (params->flags & HDRG_WF_MID) != 0u;
    const void *mapped;
    hdrg_wf_t *wf = work->wf;
    WGPUBindGroupEntry entries[HDRG_WF_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WF_BINDINGS];
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    uint32_t i, groups_x, groups_y;
    bool ok;

    if (!hdrg_wf_buffers(gpu, wf, in_size, mid_size, out_size)) {
        return HDRG_ERR_NOMEM;
    }

    buffers[0] = wf->params;
    buffers[1] = wf->input;
    buffers[2] = wf->mid;
    buffers[3] = wf->output;

    for (i = 0; i < HDRG_WF_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = wf->group_layout;
    group_desc.entryCount = HDRG_WF_BINDINGS;
    group_desc.entries = entries;
    groups_x = ((params->out_w + 1u) / 2u + HDRG_WF_GROUP - 1u) / HDRG_WF_GROUP;
    groups_y = (params->out_h + HDRG_WF_GROUP - 1u) / HDRG_WF_GROUP;

    hdrg_gpu_scope_begin(gpu);
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wf->params, 0, params, sizeof(*params));
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wf->input, 0, work->in, in_size);
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderSetPipeline(pass, wf->easu);
    wgpuComputePassEncoderDispatchWorkgroups(pass, groups_x, groups_y, 1);

    if (sharpen) {
        wgpuComputePassEncoderSetPipeline(pass, wf->rcas);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups_x, groups_y, 1);
    }

    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, wf->output, 0, wf->readback, 0, out_size);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(group);
    ok = hdrg_gpu_scope_end(gpu);

    mapped = ok ? hdrg_gpu_map_read(gpu, wf->readback, out_size) : NULL;
    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->out, mapped, out_size);
    wgpuBufferUnmap(wf->readback);

    return HDRG_OK;
}

static void hdrg_wf_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wf_work_t *work = (hdrg_wf_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_wf_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static void hdrg_wf_free(hdrg_wf_t *wf, const hdrg_plugin_api_t *api)
{
    if (wf->gpu != NULL) {
        (void)hdrg_gpu_call(wf->gpu, hdrg_wf_release, wf);
    }

    hdrg_gpu_destroy(wf->gpu);
    api->mem_free(api, wf);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wf_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wf_t *wf;
    hdrg_status_t status;
    int64_t sharpen;
    double stops;

    wf = (hdrg_wf_t *)api->mem_calloc(api, 1, sizeof(*wf));
    if (wf == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_int(options, HDRG_WF_LIT("sharpen"), &sharpen);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_WF_LIT("sharpness"), &stops);
    }

    if (status == HDRG_OK) {
        wf->sharpen = sharpen != 0;
        wf->sharpness = hdrg_math_exp2f(-(float)stops);
        status = api->filter_set_max_inflight(filter, HDRG_WF_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &wf->gpu);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(wf->gpu, hdrg_wf_build, wf)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !wf->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_wf_free(wf, api);

        return status;
    }

    *inst = wf;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wf_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wf_free((hdrg_wf_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wf_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wf_t *wf = (hdrg_wf_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &wf->in_w, &wf->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &wf->out_w, &wf->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || wf->in_w == 0 || wf->in_h == 0 || wf->out_w < wf->in_w ||
        wf->out_h < wf->in_h) {
        return HDRG_ERR_UNSUPPORTED;
    }

    hdrg_fsr_easu_con(wf->in_w, wf->out_w, &wf->scale[0], &wf->offset[0]);
    hdrg_fsr_easu_con(wf->in_h, wf->out_h, &wf->scale[1], &wf->offset[1]);

    return api->nego_set_output(nego, wf->out_w, wf->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wf_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wf_t *wf = (hdrg_wf_t *)inst;
    hdrg_wf_work_t *work;
    hdrg_wf_params_t *params;
    hdrg_status_t status;
    uint32_t begin, end, row_begin, row_end;
    bool same;

    work = (hdrg_wf_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wf = wf;
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
        (begin != 0u || end != wf->out_h || row_begin != 0u || row_end != wf->in_h ||
         work->in_stride % 4u != 0u || work->out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    same = wf->out_w == wf->in_w && wf->out_h == wf->in_h;
    params = &work->params;
    params->in_w = wf->in_w;
    params->in_h = wf->in_h;
    params->out_w = wf->out_w;
    params->out_h = wf->out_h;
    params->in_stride = (uint32_t)(work->in_stride / 4u);
    params->out_stride = (uint32_t)(work->out_stride / 4u);
    params->flags = same ? HDRG_WF_COPY : wf->sharpen ? HDRG_WF_MID : 0u;
    params->scale_x = wf->scale[0];
    params->scale_y = wf->scale[1];
    params->offset_x = wf->offset[0];
    params->offset_y = wf->offset[1];
    params->sharpness = wf->sharpness;

    if (!hdrg_gpu_run(wf->gpu, hdrg_wf_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_fsr_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_WF_LIT(HDRG_WF_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_int(cls, HDRG_WF_LIT("sharpen"), 0, 0, 1,
                                       HDRG_WF_LIT("RCAS: sharpen EASU's output"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_WF_LIT("sharpness"), 0.2, 0.0, 2.0,
            HDRG_WF_LIT("RCAS's sharpness in stops below the strongest (0 = the strongest)"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_wf_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_wf_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_wf_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_wf_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
