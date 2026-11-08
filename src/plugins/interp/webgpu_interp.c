/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/plugins/blockme.h"
#include "parts.h"

#define HDRG_WI_ID         "hdrg.webgpu.interp.mc"
#define HDRG_WI_LIT(s)     s, sizeof(s) - 1u
#define HDRG_WI_BINDINGS   8u
#define HDRG_WI_GROUP      8u   /* the shader's workgroups: 8 x 8 */
#define HDRG_WI_TABLE      8u   /* the shader's table: words a level ... */
#define HDRG_WI_SCRATCH    48u  /* ... and the scratch's place */
#define HDRG_WI_SLOT       256u /* a dispatch's uniform in the buffer: the dynamic offsets' unit */
#define HDRG_WI_DISPATCHES (3u * HDRG_BME_LEVELS + 2u)
#define HDRG_WI_SUBMITTED  (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)

/*
 * hdrg.interp.mc (src/plugins/interp/interp_mc.c) on the GPU
 * (src/plugins/interp/wgsl/interp.wgsl): the CPU's levels, integers and order,
 * the same output; the two RGB16 frames uploaded, the luma, the pyramid,
 * a search and a median each level coarse to fine and the output in one
 * pass (each dispatch's level its uniform's slot, a dynamic offset), the
 * output and the finest field's records read back: the output's
 * HDRG_SD_MOTION before job_done. ASYNC: the job runs on the device's
 * thread (src/gpu/webgpu), a frame in flight.
 */
typedef struct {
    hdrg_gpu_t *gpu;
    WGPUBindGroupLayout group_layout; /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline lumas, down, coarse, refine, median, compose, confidence;
    WGPUBuffer params, frame_a, frame_b, pyramid, fields, table, output, records, readback;
    uint8_t *side; /* the output's HDRG_SD_MOTION (side_size bytes) */
    size_t side_size;
    size_t a_size, b_size, out_size;    /* the frame buffers' (0: none yet) */
    size_t records_size, readback_size; /* the records' (the fit's), the read-back's */
    uint64_t pyramid_words, field_words;
    uint32_t table_words[HDRG_WI_SCRATCH + 1u];
    uint32_t width, height, levels; /* configure's (levels 0: none yet) */
    uint32_t sizes[HDRG_BME_LEVELS][2], blocks[HDRG_BME_LEVELS][2];
    bool built;  /* create: the pipelines */
    bool fitted; /* the device's thread: the pyramid, the fields and the table for the size */
} hdrg_wi_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, a_stride, b_stride, out_stride, phase, lv, levels;
} hdrg_wi_params_t;

/* a job as submitted: what its run needs */
typedef struct {
    hdrg_wi_t *wi;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    const uint8_t *a, *b;
    uint8_t *out;
    size_t a_stride, b_stride, out_stride;
    uint32_t phase;
} hdrg_wi_work_t;

static uint32_t hdrg_wi_div(uint32_t v, uint32_t d)
{
    return (v + d - 1u) / d;
}

static void hdrg_wi_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

static void hdrg_wi_release_pipeline(WGPUComputePipeline *pipeline)
{
    if (*pipeline != NULL) {
        wgpuComputePipelineRelease(*pipeline);
        *pipeline = NULL;
    }
}

/* the device's thread: the layouts and the pipelines */
static void hdrg_wi_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WI_BINDINGS] = {
        WGPUBufferBindingType_Uniform,         WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Storage,
        WGPUBufferBindingType_Storage,         WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_Storage,         WGPUBufferBindingType_Storage};
    hdrg_wi_t *wi = (hdrg_wi_t *)arg;
    WGPUBindGroupLayoutEntry entries[HDRG_WI_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;

    for (i = 0; i < HDRG_WI_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    entries[0].buffer.hasDynamicOffset = 1;
    entries[0].buffer.minBindingSize = sizeof(hdrg_wi_params_t);
    group_desc.entryCount = HDRG_WI_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    wi->group_layout = wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &wi->group_layout;
    wi->layout = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_interp);
    if (module == NULL) {
        return;
    }

    wi->lumas = hdrg_gpu_pipeline(gpu, module, "lumas", wi->layout);
    wi->down = hdrg_gpu_pipeline(gpu, module, "down", wi->layout);
    wi->coarse = hdrg_gpu_pipeline(gpu, module, "coarse", wi->layout);
    wi->refine = hdrg_gpu_pipeline(gpu, module, "refine", wi->layout);
    wi->median = hdrg_gpu_pipeline(gpu, module, "median", wi->layout);
    wi->compose = hdrg_gpu_pipeline(gpu, module, "compose", wi->layout);
    wi->confidence = hdrg_gpu_pipeline(gpu, module, "confidence", wi->layout);
    wi->params = hdrg_gpu_buffer(gpu, (size_t)HDRG_WI_DISPATCHES * HDRG_WI_SLOT,
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);

    wi->built = wi->lumas != NULL && wi->down != NULL && wi->coarse != NULL && wi->refine != NULL &&
                wi->median != NULL && wi->compose != NULL && wi->confidence != NULL &&
                wi->params != NULL;
}

/* the device's thread: every object */
static void hdrg_wi_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wi_t *wi = (hdrg_wi_t *)arg;

    (void)gpu;

    hdrg_wi_release_pipeline(&wi->lumas);
    hdrg_wi_release_pipeline(&wi->down);
    hdrg_wi_release_pipeline(&wi->coarse);
    hdrg_wi_release_pipeline(&wi->refine);
    hdrg_wi_release_pipeline(&wi->median);
    hdrg_wi_release_pipeline(&wi->compose);
    hdrg_wi_release_pipeline(&wi->confidence);

    if (wi->layout != NULL) {
        wgpuPipelineLayoutRelease(wi->layout);
    }

    if (wi->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(wi->group_layout);
    }

    hdrg_wi_release_buffer(&wi->params);
    hdrg_wi_release_buffer(&wi->frame_a);
    hdrg_wi_release_buffer(&wi->frame_b);
    hdrg_wi_release_buffer(&wi->pyramid);
    hdrg_wi_release_buffer(&wi->fields);
    hdrg_wi_release_buffer(&wi->table);
    hdrg_wi_release_buffer(&wi->output);
    hdrg_wi_release_buffer(&wi->records);
    hdrg_wi_release_buffer(&wi->readback);
}

/* the device's thread: the pyramid, the fields and the table for the configured size */
static void hdrg_wi_fit(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wi_t *wi = (hdrg_wi_t *)arg;

    hdrg_wi_release_buffer(&wi->pyramid);
    hdrg_wi_release_buffer(&wi->fields);
    hdrg_wi_release_buffer(&wi->table);
    hdrg_wi_release_buffer(&wi->records);
    wi->pyramid = hdrg_gpu_buffer(gpu, (size_t)wi->pyramid_words * 4u, WGPUBufferUsage_Storage);
    wi->fields = hdrg_gpu_buffer(gpu, (size_t)wi->field_words * 4u, WGPUBufferUsage_Storage);
    wi->table = hdrg_gpu_buffer(gpu, sizeof(wi->table_words), HDRG_WI_SUBMITTED);
    wi->records =
        hdrg_gpu_buffer(gpu, wi->records_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
    wi->fitted =
        wi->pyramid != NULL && wi->fields != NULL && wi->table != NULL && wi->records != NULL;

    if (wi->fitted) {
        wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wi->table, 0, wi->table_words,
                             sizeof(wi->table_words));
    }
}

/* the levels of hdrg_mc_fit for width x height and their table (the buffers then made for them) */
static bool hdrg_wi_levels(hdrg_wi_t *wi, uint32_t width, uint32_t height)
{
    hdrg_bme_t shape;
    uint64_t pyramid, fields;
    uint32_t count, level, *row;

    hdrg_bme_shape(&shape, width, height);
    count = shape.levels;

    for (level = 0; level < count; level++) {
        wi->sizes[level][0] = shape.sizes[level][0];
        wi->sizes[level][1] = shape.sizes[level][1];
        wi->blocks[level][0] = shape.blocks_w[level];
        wi->blocks[level][1] = shape.blocks_h[level];
    }

    memset(wi->table_words, 0, sizeof(wi->table_words));
    pyramid = 0;
    fields = 0;

    /* each level's size, its planes (the first frame's, the second's), its blocks, its field */
    for (level = 0; level < count; level++) {
        row = wi->table_words + level * HDRG_WI_TABLE;
        row[0] = wi->sizes[level][0];
        row[1] = wi->sizes[level][1];
        row[2] = (uint32_t)pyramid;
        row[3] = (uint32_t)(pyramid + (uint64_t)row[0] * row[1]);
        row[4] = wi->blocks[level][0];
        row[5] = wi->blocks[level][1];
        row[6] = (uint32_t)fields;
        pyramid += 2u * (uint64_t)row[0] * row[1];
        fields += 2u * (uint64_t)row[4] * row[5];
    }

    wi->table_words[HDRG_WI_SCRATCH] = (uint32_t)fields;
    fields += 2u * (uint64_t)wi->blocks[0][0] * wi->blocks[0][1];
    wi->pyramid_words = pyramid;
    wi->field_words = fields;
    wi->levels = count;
    wi->records_size = (size_t)wi->blocks[0][0] * wi->blocks[0][1] * 2u * sizeof(uint32_t);

    return pyramid <= UINT32_MAX && fields <= UINT32_MAX && pyramid * 4u <= (uint64_t)SIZE_MAX &&
           fields * 4u <= (uint64_t)SIZE_MAX;
}

/* the device's thread: a buffer for size bytes of usage, kept while large enough */
static bool hdrg_wi_buffer(hdrg_gpu_t *gpu, WGPUBuffer *buffer, size_t *have, size_t size,
                           WGPUBufferUsage usage)
{
    if (size > *have) {
        hdrg_wi_release_buffer(buffer);
        *buffer = hdrg_gpu_buffer(gpu, size, usage);
        *have = (*buffer != NULL) ? size : 0u;
    }

    return *have >= size;
}

/* the device's thread: a job's uploads, dispatches and read-back */
static hdrg_status_t hdrg_wi_frame(hdrg_gpu_t *gpu, const hdrg_wi_work_t *work)
{
    const size_t a_size = work->a_stride * work->wi->height,
                 b_size = work->b_stride * work->wi->height,
                 out_size = work->out_stride * work->wi->height;
    const void *mapped;
    hdrg_wi_t *wi = work->wi;
    hdrg_wi_params_t slots[HDRG_WI_DISPATCHES * (HDRG_WI_SLOT / sizeof(hdrg_wi_params_t))], *params;
    WGPUBindGroupEntry entries[HDRG_WI_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WI_BINDINGS];
    WGPUComputePipeline pipelines[HDRG_WI_DISPATCHES];
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    hdrg_status_t status;
    uint32_t groups[HDRG_WI_DISPATCHES][2], i, level, count, offset;
    bool ok;

    if (!hdrg_wi_buffer(gpu, &wi->frame_a, &wi->a_size, a_size, HDRG_WI_SUBMITTED) ||
        !hdrg_wi_buffer(gpu, &wi->frame_b, &wi->b_size, b_size, HDRG_WI_SUBMITTED)) {
        return HDRG_ERR_NOMEM;
    }

    if (out_size > wi->out_size) {
        hdrg_wi_release_buffer(&wi->output);
        wi->output =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        wi->out_size = (wi->output != NULL) ? out_size : 0u;
    }

    /* the read-back: the output, then the records */
    if (!hdrg_wi_buffer(gpu, &wi->readback, &wi->readback_size, out_size + wi->records_size,
                        WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst) ||
        wi->out_size < out_size) {
        return HDRG_ERR_NOMEM;
    }

    /* the dispatches in order: each its pipeline, workgroups and uniform's slot */
    memset(slots, 0, sizeof(slots));
    count = 0;
    pipelines[count] = wi->lumas;
    groups[count][0] = hdrg_wi_div(wi->sizes[0][0], HDRG_WI_GROUP);
    groups[count][1] = hdrg_wi_div(wi->sizes[0][1], HDRG_WI_GROUP);
    count++;

    for (level = 1; level < wi->levels; level++) {
        pipelines[count] = wi->down;
        groups[count][0] = hdrg_wi_div(wi->sizes[level][0], HDRG_WI_GROUP);
        groups[count][1] = hdrg_wi_div(wi->sizes[level][1], HDRG_WI_GROUP);
        slots[count * (HDRG_WI_SLOT / sizeof(hdrg_wi_params_t))].lv = level;
        count++;
    }

    for (level = wi->levels; level-- > 0;) {
        for (i = 0; i < 2u; i++) {
            pipelines[count] = (i == 1u)                    ? wi->median
                               : (level + 1u == wi->levels) ? wi->coarse
                                                            : wi->refine;
            groups[count][0] = hdrg_wi_div(wi->blocks[level][0], HDRG_WI_GROUP);
            groups[count][1] = hdrg_wi_div(wi->blocks[level][1], HDRG_WI_GROUP);
            slots[count * (HDRG_WI_SLOT / sizeof(hdrg_wi_params_t))].lv = level;
            count++;
        }
    }

    pipelines[count] = wi->compose;
    groups[count][0] = hdrg_wi_div(hdrg_wi_div(wi->width, 2u), HDRG_WI_GROUP);
    groups[count][1] = hdrg_wi_div(wi->height, HDRG_WI_GROUP);
    count++;
    pipelines[count] = wi->confidence;
    groups[count][0] = hdrg_wi_div(wi->blocks[0][0], HDRG_WI_GROUP);
    groups[count][1] = hdrg_wi_div(wi->blocks[0][1], HDRG_WI_GROUP);
    count++;

    for (i = 0; i < count; i++) {
        params = &slots[i * (HDRG_WI_SLOT / sizeof(hdrg_wi_params_t))];
        params->width = wi->width;
        params->height = wi->height;
        params->a_stride = (uint32_t)(work->a_stride / 4u);
        params->b_stride = (uint32_t)(work->b_stride / 4u);
        params->out_stride = (uint32_t)(work->out_stride / 4u);
        params->phase = work->phase;
        params->levels = wi->levels;
    }

    buffers[0] = wi->params;
    buffers[1] = wi->frame_a;
    buffers[2] = wi->frame_b;
    buffers[3] = wi->pyramid;
    buffers[4] = wi->fields;
    buffers[5] = wi->table;
    buffers[6] = wi->output;
    buffers[7] = wi->records;

    for (i = 0; i < HDRG_WI_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = (i == 0u) ? sizeof(hdrg_wi_params_t) : WGPU_WHOLE_SIZE;
    }

    group_desc.layout = wi->group_layout;
    group_desc.entryCount = HDRG_WI_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wi->params, 0, slots, (size_t)count * HDRG_WI_SLOT);
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wi->frame_a, 0, work->a, a_size);
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wi->frame_b, 0, work->b, b_size);
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);

    for (i = 0; i < count; i++) {
        offset = i * HDRG_WI_SLOT;
        wgpuComputePassEncoderSetBindGroup(pass, 0, group, 1, &offset);
        wgpuComputePassEncoderSetPipeline(pass, pipelines[i]);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups[i][0], groups[i][1], 1);
    }

    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, wi->output, 0, wi->readback, 0, out_size);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, wi->records, 0, wi->readback, out_size,
                                         wi->records_size);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(group);

    ok = hdrg_gpu_scope_end(gpu);
    mapped = ok ? hdrg_gpu_map_read(gpu, wi->readback, out_size + wi->records_size) : NULL;

    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->out, mapped, out_size);
    hdrg_interp_motion_head(wi->blocks[0][0], wi->blocks[0][1], work->phase, wi->side);
    hdrg_interp_motion_records((const uint8_t *)mapped + out_size, wi->blocks[0][0],
                               wi->blocks[0][1], wi->side);
    wgpuBufferUnmap(wi->readback);

    /* a host without the output's side data (none asked for) takes the frame alone */
    status = work->api->job_set_side_data(work->job, HDRG_SD_MOTION, HDRG_SD_MOTION_VERSION,
                                          wi->side, wi->side_size);

    return (status == HDRG_ERR_UNSUPPORTED) ? HDRG_OK : status;
}

static void hdrg_wi_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wi_work_t *work = (hdrg_wi_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_wi_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static void hdrg_wi_free(hdrg_wi_t *wi, const hdrg_plugin_api_t *api)
{
    if (wi->gpu != NULL) {
        (void)hdrg_gpu_call(wi->gpu, hdrg_wi_release, wi);
    }

    api->mem_free(api, wi->side);
    hdrg_gpu_destroy(wi->gpu);
    api->mem_free(api, wi);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wi_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wi_t *wi;
    hdrg_status_t status;

    (void)options;

    wi = (hdrg_wi_t *)api->mem_calloc(api, 1, sizeof(*wi));
    if (wi == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->filter_set_max_inflight(filter, 1);
    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &wi->gpu);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(wi->gpu, hdrg_wi_build, wi)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !wi->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_wi_free(wi, api);

        return status;
    }

    *inst = wi;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wi_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wi_free((hdrg_wi_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wi_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wi_t *wi = (hdrg_wi_t *)inst;
    hdrg_status_t status;
    uint32_t width, height, format;

    status = api->nego_input(nego, &width, &height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || width == 0 || height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    /* the levels and their buffers, made once (a stage's size stays) */
    if (!wi->fitted || width != wi->width || height != wi->height) {
        wi->fitted = false;
        api->mem_free(api, wi->side);
        wi->side = NULL;

        if (!hdrg_wi_levels(wi, width, height) || !hdrg_gpu_call(wi->gpu, hdrg_wi_fit, wi) ||
            !wi->fitted) {
            return HDRG_ERR_NOMEM;
        }

        wi->side_size = hdrg_bme_motion_size(wi->blocks[0][0], wi->blocks[0][1], 1u);
        wi->side = (uint8_t *)api->mem_alloc(api, wi->side_size);
        if (wi->side == NULL) {
            wi->fitted = false;

            return HDRG_ERR_NOMEM;
        }

        wi->width = width;
        wi->height = height;
    }

    return api->nego_set_output(nego, width, height, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wi_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wi_t *wi = (hdrg_wi_t *)inst;
    hdrg_wi_work_t *work;
    hdrg_status_t status;
    uint32_t begin, end;

    work = (hdrg_wi_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wi = wi;
    work->job = job;
    work->api = api;
    status = api->job_input(job, 0, &work->a, &work->a_stride, &begin, &end);

    if (status == HDRG_OK) {
        status = api->job_input_frame(job, 1, 0, &work->b, &work->b_stride, &begin, &end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &work->out, &work->out_stride);
    }

    if (status == HDRG_OK) {
        status = api->job_phase(job, &work->phase);
    }

    if (status == HDRG_OK && (work->a_stride % 4u != 0u || work->b_stride % 4u != 0u ||
                              work->out_stride % 4u != 0u || work->phase > 65536u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    if (!hdrg_gpu_run(wi->gpu, hdrg_wi_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_interp_register(hdrg_plugin_t *plugin,
                                                 const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_WI_LIT(HDRG_WI_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_INTERP);
    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_DETERMINISTIC |
                                               HDRG_FILTER_ASYNC | HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_wi_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_wi_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_wi_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_wi_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
