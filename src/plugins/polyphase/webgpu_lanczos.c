/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_WL_ID        "hdrg.webgpu.lanczos"
#define HDRG_WL_LIT(s)    s, sizeof(s) - 1u
#define HDRG_WL_INFLIGHT  2u /* frames in flight: the next one's upload meets this one's */
#define HDRG_WL_BINDINGS  5u
#define HDRG_WL_GROUP     8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WL_SUBMITTED (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)

/*
 * hdrg.scale.lanczos (src/plugins/polyphase/polyphase.c) on the GPU
 * (src/plugins/polyphase/wgsl/lanczos.wgsl): the CPU's tables (hdrg_polyphase_axis)
 * and integers, the same output; whole RGB16 frames uploaded, resampled in
 * two dispatches and read back. ASYNC: the jobs run in order on the device's
 * thread (src/gpu/webgpu), HDRG_WL_INFLIGHT of them in flight.
 */
typedef struct {
    hdrg_gpu_t *gpu;
    WGPUBindGroupLayout group_layout; /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline vertical, horizontal;
    WGPUBuffer params, input, taps, mid, output, readback;
    size_t in_size, mid_size, out_size; /* the buffers' (0: none yet) */
    hdrg_polyphase_axis_t rows, cols;   /* configure's */
    int32_t *table;                     /* the tables packed, while configure uploads them */
    size_t table_count;
    uint32_t offsets[6];
    uint32_t in_w, in_h, out_w, out_h; /* configure's */
    int32_t antiring;                  /* Q8 */
    bool built;                        /* create: the pipelines */
    int64_t lobes;
} hdrg_wl_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h, in_stride, out_stride, row_taps, col_taps;
    uint32_t rows_index, rows_weight, rows_near, cols_index, cols_weight, cols_near;
    int32_t antiring;
    uint32_t flags;
} hdrg_wl_params_t;

/* a job as submitted: what its run needs */
typedef struct {
    hdrg_wl_t *wl;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    const uint8_t *in;
    uint8_t *out;
    size_t in_stride, out_stride;
    hdrg_wl_params_t params;
} hdrg_wl_work_t;

static void hdrg_wl_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

/* the device's thread: the layouts and the two pipelines */
static void hdrg_wl_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WL_BINDINGS] = {
        WGPUBufferBindingType_Uniform, WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Storage,
        WGPUBufferBindingType_Storage};
    hdrg_wl_t *wl = (hdrg_wl_t *)arg;
    WGPUBindGroupLayoutEntry entries[HDRG_WL_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;

    for (i = 0; i < HDRG_WL_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    group_desc.entryCount = HDRG_WL_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    wl->group_layout = wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &wl->group_layout;
    wl->layout = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_lanczos);
    if (module == NULL) {
        return;
    }

    wl->vertical = hdrg_gpu_pipeline(gpu, module, "vertical", wl->layout);
    wl->horizontal = hdrg_gpu_pipeline(gpu, module, "horizontal", wl->layout);
    wl->params = hdrg_gpu_buffer(gpu, sizeof(hdrg_wl_params_t),
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);

    wl->built = wl->vertical != NULL && wl->horizontal != NULL && wl->params != NULL;
}

/* the device's thread: every object */
static void hdrg_wl_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wl_t *wl = (hdrg_wl_t *)arg;

    (void)gpu;

    if (wl->vertical != NULL) {
        wgpuComputePipelineRelease(wl->vertical);
    }

    if (wl->horizontal != NULL) {
        wgpuComputePipelineRelease(wl->horizontal);
    }

    if (wl->layout != NULL) {
        wgpuPipelineLayoutRelease(wl->layout);
    }

    if (wl->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(wl->group_layout);
    }

    hdrg_wl_release_buffer(&wl->params);
    hdrg_wl_release_buffer(&wl->input);
    hdrg_wl_release_buffer(&wl->taps);
    hdrg_wl_release_buffer(&wl->mid);
    hdrg_wl_release_buffer(&wl->output);
    hdrg_wl_release_buffer(&wl->readback);
}

/* the device's thread: the configured tables uploaded (once their sizes are known) */
static void hdrg_wl_upload(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wl_t *wl = (hdrg_wl_t *)arg;
    size_t size;

    size = wl->table_count * sizeof(int32_t);
    hdrg_wl_release_buffer(&wl->taps);
    wl->taps = hdrg_gpu_buffer(gpu, size, HDRG_WL_SUBMITTED);

    if (wl->taps != NULL) {
        wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wl->taps, 0, wl->table, size);
    }
}

/* the device's thread: the frame buffers for a job's sizes, kept while they are large enough */
static bool hdrg_wl_buffers(hdrg_gpu_t *gpu, hdrg_wl_t *wl, size_t in_size, size_t mid_size,
                            size_t out_size)
{
    if (in_size > wl->in_size) {
        hdrg_wl_release_buffer(&wl->input);
        wl->input = hdrg_gpu_buffer(gpu, in_size, HDRG_WL_SUBMITTED);
        wl->in_size = (wl->input != NULL) ? in_size : 0u;
    }

    if (mid_size > wl->mid_size) {
        hdrg_wl_release_buffer(&wl->mid);
        wl->mid = hdrg_gpu_buffer(gpu, mid_size, WGPUBufferUsage_Storage);
        wl->mid_size = (wl->mid != NULL) ? mid_size : 0u;
    }

    if (out_size > wl->out_size) {
        hdrg_wl_release_buffer(&wl->output);
        hdrg_wl_release_buffer(&wl->readback);
        wl->output =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        wl->readback =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        wl->out_size = (wl->output != NULL && wl->readback != NULL) ? out_size : 0u;
    }

    return wl->in_size >= in_size && wl->mid_size >= mid_size && wl->out_size >= out_size;
}

/* the device's thread: a job's upload, dispatches and read-back */
static hdrg_status_t hdrg_wl_frame(hdrg_gpu_t *gpu, const hdrg_wl_work_t *work)
{
    const hdrg_wl_params_t *params = &work->params;
    const size_t in_size = work->in_stride * params->in_h,
                 mid_size = (size_t)params->out_h * params->in_w * 3u * sizeof(int32_t),
                 out_size = work->out_stride * params->out_h;
    const void *mapped;
    hdrg_wl_t *wl = work->wl;
    WGPUBindGroupEntry entries[HDRG_WL_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WL_BINDINGS];
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    uint32_t i, groups_y;
    bool ok;

    if (!hdrg_wl_buffers(gpu, wl, in_size, mid_size, out_size)) {
        return HDRG_ERR_NOMEM;
    }

    buffers[0] = wl->params;
    buffers[1] = wl->input;
    buffers[2] = wl->taps;
    buffers[3] = wl->mid;
    buffers[4] = wl->output;

    for (i = 0; i < HDRG_WL_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = wl->group_layout;
    group_desc.entryCount = HDRG_WL_BINDINGS;
    group_desc.entries = entries;
    groups_y = (params->out_h + HDRG_WL_GROUP - 1u) / HDRG_WL_GROUP;

    hdrg_gpu_scope_begin(gpu);

    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wl->params, 0, params, sizeof(*params));
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wl->input, 0, work->in, in_size);
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderSetPipeline(pass, wl->vertical);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (params->in_w + HDRG_WL_GROUP - 1u) / HDRG_WL_GROUP, groups_y, 1);
    wgpuComputePassEncoderSetPipeline(pass, wl->horizontal);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, ((params->out_w + 1u) / 2u + HDRG_WL_GROUP - 1u) / HDRG_WL_GROUP, groups_y, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, wl->output, 0, wl->readback, 0, out_size);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(group);

    ok = hdrg_gpu_scope_end(gpu);
    mapped = ok ? hdrg_gpu_map_read(gpu, wl->readback, out_size) : NULL;

    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->out, mapped, out_size);
    wgpuBufferUnmap(wl->readback);

    return HDRG_OK;
}

static void hdrg_wl_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wl_work_t *work = (hdrg_wl_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_wl_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static void hdrg_wl_free(hdrg_wl_t *wl, const hdrg_plugin_api_t *api)
{
    if (wl->gpu != NULL) {
        (void)hdrg_gpu_call(wl->gpu, hdrg_wl_release, wl);
    }

    hdrg_polyphase_axis_free(api, &wl->rows);
    hdrg_polyphase_axis_free(api, &wl->cols);
    hdrg_gpu_destroy(wl->gpu);
    api->mem_free(api, wl);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wl_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wl_t *wl;
    hdrg_status_t status;
    double strength;

    wl = (hdrg_wl_t *)api->mem_calloc(api, 1, sizeof(*wl));
    if (wl == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_int(options, HDRG_WL_LIT("taps"), &wl->lobes);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_WL_LIT("antiring"), &strength);
    }

    if (status == HDRG_OK) {
        wl->antiring = (int32_t)floor(strength * (double)(1u << HDRG_POLYPHASE_AR) + 0.5);
        status = api->filter_set_max_inflight(filter, HDRG_WL_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &wl->gpu);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(wl->gpu, hdrg_wl_build, wl)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !wl->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_wl_free(wl, api);

        return status;
    }

    *inst = wl;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wl_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wl_free((hdrg_wl_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wl_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wl_t *wl = (hdrg_wl_t *)inst;
    hdrg_status_t status;
    uint32_t format, in_w, in_h, out_w, out_h;
    bool ok;

    status = api->nego_input(nego, &in_w, &in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &out_w, &out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || in_w == 0 || in_h == 0 || out_w == 0 || out_h == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    /* the tables of the sizes, made and uploaded once (a stage's sizes stay) */
    if (in_w != wl->in_w || in_h != wl->in_h || out_w != wl->out_w || out_h != wl->out_h) {
        status = hdrg_polyphase_axis(api, (uint8_t)wl->lobes, 0.0, 0.0, in_w, out_w, &wl->cols);
        if (status == HDRG_OK) {
            status = hdrg_polyphase_axis(api, (uint8_t)wl->lobes, 0.0, 0.0, in_h, out_h, &wl->rows);
        }

        if (status != HDRG_OK) {
            return status;
        }

        wl->table = hdrg_polyphase_pack(api, &wl->rows, &wl->cols, out_w, out_h, wl->offsets,
                                        &wl->table_count);
        ok = wl->table != NULL && hdrg_gpu_call(wl->gpu, hdrg_wl_upload, wl) && wl->taps != NULL;
        api->mem_free(api, wl->table);
        wl->table = NULL;

        if (!ok) {
            return HDRG_ERR_NOMEM;
        }

        wl->in_w = in_w;
        wl->in_h = in_h;
        wl->out_w = out_w;
        wl->out_h = out_h;
    }

    return api->nego_set_output(nego, out_w, out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wl_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wl_t *wl = (hdrg_wl_t *)inst;
    hdrg_wl_work_t *work;
    hdrg_wl_params_t *params;
    hdrg_status_t status;
    uint32_t begin, end, row_begin, row_end;

    work = (hdrg_wl_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wl = wl;
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
        (begin != 0u || end != wl->out_h || row_begin != 0u || row_end != wl->in_h ||
         work->in_stride % 4u != 0u || work->out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    params = &work->params;
    params->in_w = wl->in_w;
    params->in_h = wl->in_h;
    params->out_w = wl->out_w;
    params->out_h = wl->out_h;
    params->in_stride = (uint32_t)(work->in_stride / 4u);
    params->out_stride = (uint32_t)(work->out_stride / 4u);
    params->row_taps = wl->rows.taps;
    params->col_taps = wl->cols.taps;
    params->rows_index = wl->offsets[0];
    params->rows_weight = wl->offsets[1];
    params->rows_near = wl->offsets[2];
    params->cols_index = wl->offsets[3];
    params->cols_weight = wl->offsets[4];
    params->cols_near = wl->offsets[5];
    params->antiring = wl->antiring;
    params->flags = ((wl->rows.upscale && wl->antiring > 0) ? 1u : 0u) |
                    ((wl->cols.upscale && wl->antiring > 0) ? 2u : 0u);

    if (!hdrg_gpu_run(wl->gpu, hdrg_wl_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_lanczos_register(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_WL_LIT(HDRG_WL_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_int(cls, HDRG_WL_LIT("taps"), 3, 2, 4,
                                       HDRG_WL_LIT("lobes of the Lanczos kernel"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_WL_LIT("antiring"), 1.0, 0.0, 1.0,
            HDRG_WL_LIT("when upscaling, pull each sample toward the range of the source samples "
                        "around it (0 = off)"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_wl_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_wl_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_wl_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_wl_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
