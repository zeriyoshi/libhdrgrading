/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "mctf.h"
#include "parts.h"

#define HDRG_WM_ID     "hdrg.webgpu.mctf"
#define HDRG_WM_LIT(s) s, sizeof(s) - 1u
#define HDRG_WM_GROUP  8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WM_REACH  HDRG_MCTF_REACH
#define HDRG_WM_REFS   HDRG_MCTF_REFS  /* the window's frames besides the job's ... */
#define HDRG_WM_SLOTS  HDRG_MCTF_SLOTS /* ... the frames and pyramids kept, by index */
#define HDRG_WM_SLOT   256u /* a dispatch's uniform in the buffer: the dynamic offsets' unit */
#define HDRG_WM_STAGE  (HDRG_WM_SLOT / sizeof(hdrg_wm_params_t)) /* the uniforms a slot */
#define HDRG_WM_DISPATCHES                                                                         \
    (HDRG_WM_SLOTS * HDRG_BME_LEVELS + HDRG_WM_REFS * (2u * HDRG_BME_LEVELS + 1u) + 1u)
#define HDRG_WM_ME_BINDINGS  5u /* the estimation's: params, src, pyramid, motion, table */
#define HDRG_WM_AVG_BINDINGS 9u /* the output's: params, motion, table, f0 .. f4, dst */
#define HDRG_WM_FRAME_USAGE  (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)
#define HDRG_WM_AVG          UINT32_MAX /* a dispatch's group: the output's (else a frame's) */

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride, lv, slot_a, slot_b, ref_index, refs, shift,
        block_lo, block_hi, w[HDRG_WM_REFS];
} hdrg_wm_params_t;

/*
 * hdrg.mctf (src/plugins/mctf/mctf.c) on the GPU (src/plugins/mctf/wgsl/mctf.wgsl):
 * the CPU's levels, integers and order, the same output. The window's RGB16
 * frames of the host are uploaded into a slot each (by frame index, kept: a
 * frame goes up once); on the device (a chain the library runs there) they
 * are the library's frames as they are. Their pyramids are made once a
 * frame index; each job one pass (the new pyramids, each other frame's
 * search and its blocks' weights, the output, each dispatch's values its
 * uniform's slot, a dynamic offset), the output read back or one of the
 * filter's device frames for the next filter. ASYNC: the job runs on the
 * device's thread (src/gpu/webgpu), a frame in flight.
 */
typedef struct {
    hdrg_wm_params_t staging[HDRG_WM_DISPATCHES * HDRG_WM_STAGE]; /* the uniforms' slots */
    hdrg_gpu_t *gpu;
    WGPUBindGroupLayout me_layout, avg_layout; /* the device's thread */
    WGPUPipelineLayout me_pipeline, avg_pipeline;
    WGPUComputePipeline lumas, down, coarse, refine, median, blocks, average;
    WGPUBuffer params, pyramid, motion, table, frames[HDRG_WM_SLOTS], output, readback,
        prefix; /* prefix: the motion's, read back for the side data */
    hdrg_webgpu_device_frame_t device[HDRG_WEBGPU_DEVICE_FRAMES]; /* to the next filter */
    hdrg_bme_t bme;                             /* the levels (hdrg_bme_shape: no fields) */
    uint8_t *side;                              /* the output's HDRG_SD_MOTION */
    size_t frame_size, out_size, readback_size; /* the buffers' (0: none yet) */
    uint64_t pyramid_words, motion_words;
    uint32_t words[HDRG_MCTF_WORDS];       /* the table, written with each job */
    uint32_t frame_index[HDRG_WM_SLOTS];   /* the frames a slot's frame (the host's) ... */
    uint32_t pyramid_index[HDRG_WM_SLOTS]; /* ... and pyramid are of */
    uint32_t block_lo, block_hi;
    uint32_t width, height; /* configure's (0: none yet) */
    uint32_t past, future;
    bool active;
    bool built;  /* create: the pipelines */
    bool fitted; /* the device's thread: the pyramid, the motion and the table for the size */
    double strength;
} hdrg_wm_t;

/* a job as submitted: what its run needs */
typedef struct {
    hdrg_wm_t *wm;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    hdrg_webgpu_job_t frames;               /* the job's frame and output */
    const uint8_t *rows[HDRG_WM_REFS + 1u]; /* the host's: the job's frame, then the others */
    WGPUBuffer devices[HDRG_WM_REFS + 1u];  /* ... on the device (the library's frames) */
    uint32_t index[HDRG_WM_REFS + 1u];
    int32_t offsets[HDRG_WM_REFS]; /* the other frames' (the side data's fields) */
    uint32_t weight[HDRG_WM_REFS];
    uint32_t count;
} hdrg_wm_work_t;

/* the frames' weights by their distance from the job's (1/256, hdrg.mctf's) */
static const uint32_t hdrg_wm_distance[HDRG_WM_REACH + 1u] = {256u, 218u, 146u};

static uint32_t hdrg_wm_div(uint32_t v, uint32_t d)
{
    return (v + d - 1u) / d;
}

static void hdrg_wm_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

static void hdrg_wm_release_pipeline(WGPUComputePipeline *pipeline)
{
    if (*pipeline != NULL) {
        wgpuComputePipelineRelease(*pipeline);
        *pipeline = NULL;
    }
}

/* a bind group layout of n entries of types at the bindings */
static WGPUBindGroupLayout hdrg_wm_layout(hdrg_gpu_t *gpu, const uint32_t *bindings,
                                          const WGPUBufferBindingType *types, uint32_t n)
{
    WGPUBindGroupLayoutEntry entries[HDRG_WM_AVG_BINDINGS];
    WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    uint32_t i;

    for (i = 0; i < n; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = bindings[i];
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    entries[0].buffer.hasDynamicOffset = 1;
    entries[0].buffer.minBindingSize = sizeof(hdrg_wm_params_t);
    desc.entryCount = n;
    desc.entries = entries;

    return wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &desc);
}

/* the device's thread: the layouts and the pipelines */
static void hdrg_wm_build(hdrg_gpu_t *gpu, void *arg)
{
    static const WGPUBufferBindingType
        me_types[HDRG_WM_ME_BINDINGS] = {WGPUBufferBindingType_Uniform,
                                         WGPUBufferBindingType_ReadOnlyStorage,
                                         WGPUBufferBindingType_Storage,
                                         WGPUBufferBindingType_Storage,
                                         WGPUBufferBindingType_ReadOnlyStorage},
        avg_types[HDRG_WM_AVG_BINDINGS] = {
            WGPUBufferBindingType_Uniform,         WGPUBufferBindingType_Storage,
            WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_ReadOnlyStorage,
            WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_ReadOnlyStorage,
            WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_ReadOnlyStorage,
            WGPUBufferBindingType_Storage};
    static const uint32_t me_bindings[HDRG_WM_ME_BINDINGS] = {0, 1, 2, 3, 4},
                          avg_bindings[HDRG_WM_AVG_BINDINGS] = {0, 3, 4, 5, 6, 7, 8, 9, 10};
    hdrg_wm_t *wm = (hdrg_wm_t *)arg;
    WGPUPipelineLayoutDescriptor desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;

    hdrg_gpu_scope_begin(gpu);

    wm->me_layout = hdrg_wm_layout(gpu, me_bindings, me_types, HDRG_WM_ME_BINDINGS);
    wm->avg_layout = hdrg_wm_layout(gpu, avg_bindings, avg_types, HDRG_WM_AVG_BINDINGS);
    desc.bindGroupLayoutCount = 1;
    desc.bindGroupLayouts = &wm->me_layout;
    wm->me_pipeline = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &desc);
    desc.bindGroupLayouts = &wm->avg_layout;
    wm->avg_pipeline = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_mctf);
    if (module == NULL) {
        return;
    }

    wm->lumas = hdrg_gpu_pipeline(gpu, module, "lumas", wm->me_pipeline);
    wm->down = hdrg_gpu_pipeline(gpu, module, "down", wm->me_pipeline);
    wm->coarse = hdrg_gpu_pipeline(gpu, module, "coarse", wm->me_pipeline);
    wm->refine = hdrg_gpu_pipeline(gpu, module, "refine", wm->me_pipeline);
    wm->median = hdrg_gpu_pipeline(gpu, module, "median", wm->me_pipeline);
    wm->blocks = hdrg_gpu_pipeline(gpu, module, "blocks", wm->me_pipeline);
    wm->average = hdrg_gpu_pipeline(gpu, module, "average", wm->avg_pipeline);
    wm->params = hdrg_gpu_buffer(gpu, (size_t)HDRG_WM_DISPATCHES * HDRG_WM_SLOT,
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);

    wm->built = wm->lumas != NULL && wm->down != NULL && wm->coarse != NULL && wm->refine != NULL &&
                wm->median != NULL && wm->blocks != NULL && wm->average != NULL &&
                wm->params != NULL;
}

/* the device's thread: every object */
static void hdrg_wm_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wm_t *wm = (hdrg_wm_t *)arg;
    uint32_t k;

    (void)gpu;

    hdrg_wm_release_pipeline(&wm->lumas);
    hdrg_wm_release_pipeline(&wm->down);
    hdrg_wm_release_pipeline(&wm->coarse);
    hdrg_wm_release_pipeline(&wm->refine);
    hdrg_wm_release_pipeline(&wm->median);
    hdrg_wm_release_pipeline(&wm->blocks);
    hdrg_wm_release_pipeline(&wm->average);

    if (wm->me_pipeline != NULL) {
        wgpuPipelineLayoutRelease(wm->me_pipeline);
    }

    if (wm->avg_pipeline != NULL) {
        wgpuPipelineLayoutRelease(wm->avg_pipeline);
    }

    if (wm->me_layout != NULL) {
        wgpuBindGroupLayoutRelease(wm->me_layout);
    }

    if (wm->avg_layout != NULL) {
        wgpuBindGroupLayoutRelease(wm->avg_layout);
    }

    hdrg_wm_release_buffer(&wm->params);
    hdrg_wm_release_buffer(&wm->pyramid);
    hdrg_wm_release_buffer(&wm->motion);
    hdrg_wm_release_buffer(&wm->table);
    hdrg_wm_release_buffer(&wm->output);
    hdrg_wm_release_buffer(&wm->readback);
    hdrg_wm_release_buffer(&wm->prefix);

    for (k = 0; k < HDRG_WM_SLOTS; k++) {
        hdrg_wm_release_buffer(&wm->frames[k]);
    }

    hdrg_webgpu_device_frames_free(wm->device);
}

/* the device's thread: the pyramid, the motion and the table for the configured size */
static void hdrg_wm_fit(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wm_t *wm = (hdrg_wm_t *)arg;
    uint32_t k;

    hdrg_wm_release_buffer(&wm->pyramid);
    hdrg_wm_release_buffer(&wm->motion);
    hdrg_wm_release_buffer(&wm->table);
    hdrg_wm_release_buffer(&wm->prefix);
    wm->pyramid = hdrg_gpu_buffer(gpu, (size_t)wm->pyramid_words * 4u, WGPUBufferUsage_Storage);
    wm->motion = hdrg_gpu_buffer(gpu, (size_t)wm->motion_words * 4u,
                                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
    wm->table = hdrg_gpu_buffer(gpu, sizeof(wm->words), HDRG_WM_FRAME_USAGE);
    wm->prefix = hdrg_gpu_buffer(gpu, hdrg_mctf_motion_prefix(&wm->bme),
                                 WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    wm->fitted =
        wm->pyramid != NULL && wm->motion != NULL && wm->table != NULL && wm->prefix != NULL;

    for (k = 0; k < HDRG_WM_SLOTS; k++) {
        wm->frame_index[k] = UINT32_MAX;
        wm->pyramid_index[k] = UINT32_MAX;
    }
}

/* the levels of hdrg_bme_shape for width x height and the table (the buffers then), the side's */
static bool hdrg_wm_levels(hdrg_wm_t *wm, uint32_t width, uint32_t height,
                           const hdrg_plugin_api_t *api)
{
    hdrg_bme_shape(&wm->bme, width, height);
    api->mem_free(api, wm->side);
    wm->side = (uint8_t *)api->mem_alloc(
        api, hdrg_bme_motion_size(wm->bme.blocks_w[0], wm->bme.blocks_h[0], HDRG_WM_REFS));

    return wm->side != NULL &&
           hdrg_mctf_table(&wm->bme, wm->words, &wm->pyramid_words, &wm->motion_words);
}

/* the device's thread: a buffer for size bytes of usage, kept while large enough */
static bool hdrg_wm_buffer(hdrg_gpu_t *gpu, WGPUBuffer *buffer, size_t *have, size_t size,
                           WGPUBufferUsage usage)
{
    if (size > *have) {
        hdrg_wm_release_buffer(buffer);
        *buffer = hdrg_gpu_buffer(gpu, size, usage);
        *have = (*buffer != NULL) ? size : 0u;
    }

    return *have >= size;
}

/* a bind group of the estimation's layout reading src as its frame */
static WGPUBindGroup hdrg_wm_me_group(hdrg_gpu_t *gpu, const hdrg_wm_t *wm, WGPUBuffer src)
{
    const WGPUBuffer buffers[HDRG_WM_ME_BINDINGS] = {wm->params, src, wm->pyramid, wm->motion,
                                                     wm->table};
    WGPUBindGroupEntry entries[HDRG_WM_ME_BINDINGS];
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    uint32_t i;

    for (i = 0; i < HDRG_WM_ME_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = (i == 0u) ? sizeof(hdrg_wm_params_t) : WGPU_WHOLE_SIZE;
    }

    desc.layout = wm->me_layout;
    desc.entryCount = HDRG_WM_ME_BINDINGS;
    desc.entries = entries;

    return wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &desc);
}

/* a dispatch of pipeline over groups with the values of params, its group group's */
static void hdrg_wm_record(hdrg_wm_params_t *staging, WGPUComputePipeline *pipelines,
                           uint32_t (*groups)[3], uint32_t *count, WGPUComputePipeline pipeline,
                           const hdrg_wm_params_t *params, uint32_t groups_x, uint32_t groups_y,
                           uint32_t group)
{
    staging[*count * HDRG_WM_STAGE] = *params;
    pipelines[*count] = pipeline;
    groups[*count][0] = groups_x;
    groups[*count][1] = groups_y;
    groups[*count][2] = group;
    (*count)++;
}

/* the device's thread: a job's uploads, dispatches and read-back (or output on the device) */
static hdrg_status_t hdrg_wm_frame(hdrg_gpu_t *gpu, const hdrg_wm_work_t *work)
{
    const size_t frame_size = work->frames.in_stride * work->wm->height,
                 out_size = work->frames.out_stride * work->wm->height;
    const bool device_in = work->frames.in_frame != NULL,
               device_out = work->frames.out_frame != NULL;
    const void *mapped;
    hdrg_wm_t *wm = work->wm;
    hdrg_wm_params_t params;
    WGPUBindGroupEntry entries[HDRG_WM_AVG_BINDINGS];
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBindGroup me_groups[HDRG_WM_REFS + 1u], avg_group;
    WGPUComputePipeline pipelines[HDRG_WM_DISPATCHES];
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    WGPUBuffer avg_buffers[HDRG_WM_AVG_BINDINGS], sources[HDRG_WM_REFS + 1u], dst;
    uint32_t groups[HDRG_WM_DISPATCHES][3], i, k, level, count, offset, slot;
    bool ok, upload[HDRG_WM_REFS + 1u];

    /* the output: one of the filter's device frames for the next filter, or read back */
    if (device_out) {
        if (!hdrg_webgpu_device_frame_fit(gpu, work->frames.out_frame, out_size)) {
            return HDRG_ERR_NOMEM;
        }

        dst = hdrg_gpu_frame_buffer(work->frames.out_frame);
    } else {
        if (!hdrg_wm_buffer(gpu, &wm->output, &wm->out_size, out_size,
                            WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc) ||
            !hdrg_wm_buffer(gpu, &wm->readback, &wm->readback_size, out_size,
                            WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst)) {
            return HDRG_ERR_NOMEM;
        }

        dst = wm->output;
    }

    /* the host's frames' slots for the window's stride, made again for another */
    if (!device_in && frame_size != wm->frame_size) {
        for (k = 0; k < HDRG_WM_SLOTS; k++) {
            hdrg_wm_release_buffer(&wm->frames[k]);
            wm->frame_index[k] = UINT32_MAX;
            wm->frames[k] = hdrg_gpu_buffer(gpu, frame_size, HDRG_WM_FRAME_USAGE);

            if (wm->frames[k] == NULL) {
                wm->frame_size = 0;

                return HDRG_ERR_NOMEM;
            }
        }

        wm->frame_size = frame_size;
    }

    /* the window's frames: on the device as they are, else the host's in their slots */
    for (k = 0; k <= work->count; k++) {
        slot = work->index[k] % HDRG_WM_SLOTS;
        sources[k] = device_in ? work->devices[k] : wm->frames[slot];
        upload[k] = !device_in && wm->frame_index[slot] != work->index[k];
    }

    memset(&params, 0, sizeof(params));
    params.width = wm->width;
    params.height = wm->height;
    params.in_stride = (uint32_t)(work->frames.in_stride / 4u);
    params.out_stride = (uint32_t)(work->frames.out_stride / 4u);
    params.refs = work->count;
    params.shift = HDRG_MCTF_SHIFT;
    params.block_lo = wm->block_lo;
    params.block_hi = wm->block_hi;
    memcpy(params.w, work->weight, sizeof(params.w));
    count = 0;

    /* the pyramids not made yet (none without other frames) */
    for (k = 0; work->count > 0u && k <= work->count; k++) {
        slot = work->index[k] % HDRG_WM_SLOTS;

        if (wm->pyramid_index[slot] == work->index[k]) {
            continue;
        }

        wm->pyramid_index[slot] = work->index[k];
        params.slot_a = slot;
        params.lv = 0;
        hdrg_wm_record(wm->staging, pipelines, groups, &count, wm->lumas, &params,
                       hdrg_wm_div(wm->bme.sizes[0][0], HDRG_WM_GROUP),
                       hdrg_wm_div(wm->bme.sizes[0][1], HDRG_WM_GROUP), k);

        for (level = 1; level < wm->bme.levels; level++) {
            params.lv = level;
            hdrg_wm_record(wm->staging, pipelines, groups, &count, wm->down, &params,
                           hdrg_wm_div(wm->bme.sizes[level][0], HDRG_WM_GROUP),
                           hdrg_wm_div(wm->bme.sizes[level][1], HDRG_WM_GROUP), k);
        }
    }

    /* each other frame: coarse to fine with each level's median, then its blocks' weights */
    for (k = 0; k < work->count; k++) {
        params.slot_a = work->index[0] % HDRG_WM_SLOTS;
        params.slot_b = work->index[k + 1u] % HDRG_WM_SLOTS;
        params.ref_index = k;

        for (level = wm->bme.levels; level-- > 0;) {
            params.lv = level;
            hdrg_wm_record(wm->staging, pipelines, groups, &count,
                           (level + 1u == wm->bme.levels) ? wm->coarse : wm->refine, &params,
                           hdrg_wm_div(wm->bme.blocks_w[level], HDRG_WM_GROUP),
                           hdrg_wm_div(wm->bme.blocks_h[level], HDRG_WM_GROUP), 0);
            hdrg_wm_record(wm->staging, pipelines, groups, &count, wm->median, &params,
                           hdrg_wm_div(wm->bme.blocks_w[level], HDRG_WM_GROUP),
                           hdrg_wm_div(wm->bme.blocks_h[level], HDRG_WM_GROUP), 0);
        }

        params.lv = 0;
        hdrg_wm_record(wm->staging, pipelines, groups, &count, wm->blocks, &params,
                       hdrg_wm_div(wm->bme.blocks_w[0], HDRG_WM_GROUP),
                       hdrg_wm_div(wm->bme.blocks_h[0], HDRG_WM_GROUP), 0);
    }

    params.lv = 0;
    hdrg_wm_record(wm->staging, pipelines, groups, &count, wm->average, &params,
                   hdrg_wm_div(hdrg_wm_div(wm->width, 2u), HDRG_WM_GROUP),
                   hdrg_wm_div(wm->height, HDRG_WM_GROUP), HDRG_WM_AVG);

    /* the output's group: the job's frame and the others (the job's again for those not read) */
    avg_buffers[0] = wm->params;
    avg_buffers[1] = wm->motion;
    avg_buffers[2] = wm->table;
    avg_buffers[3] = sources[0];

    for (k = 0; k < HDRG_WM_REFS; k++) {
        avg_buffers[4u + k] = (k < work->count) ? sources[k + 1u] : sources[0];
    }

    avg_buffers[8] = dst;

    for (i = 0; i < HDRG_WM_AVG_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = (i == 0u) ? 0u : i + 2u;
        entries[i].buffer = avg_buffers[i];
        entries[i].size = (i == 0u) ? sizeof(hdrg_wm_params_t) : WGPU_WHOLE_SIZE;
    }

    desc.layout = wm->avg_layout;
    desc.entryCount = HDRG_WM_AVG_BINDINGS;
    desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    for (k = 0; k <= work->count; k++) {
        slot = work->index[k] % HDRG_WM_SLOTS;

        if (upload[k]) {
            wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wm->frames[slot], 0, work->rows[k],
                                 frame_size);
            wm->frame_index[slot] = work->index[k];
        }
    }

    for (k = 0; k <= work->count; k++) {
        me_groups[k] = hdrg_wm_me_group(gpu, wm, sources[k]);
    }

    avg_group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &desc);
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wm->table, 0, wm->words, sizeof(wm->words));
    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wm->params, 0, wm->staging,
                         (size_t)count * HDRG_WM_SLOT);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);

    for (i = 0; i < count; i++) {
        offset = i * HDRG_WM_SLOT;
        wgpuComputePassEncoderSetBindGroup(
            pass, 0, (groups[i][2] == HDRG_WM_AVG) ? avg_group : me_groups[groups[i][2]], 1,
            &offset);
        wgpuComputePassEncoderSetPipeline(pass, pipelines[i]);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups[i][0], groups[i][1], 1);
    }

    wgpuComputePassEncoderEnd(pass);

    if (!device_out) {
        wgpuCommandEncoderCopyBufferToBuffer(encoder, wm->output, 0, wm->readback, 0, out_size);
    }

    /* the side data's part of the motion */
    if (work->count > 0u) {
        wgpuCommandEncoderCopyBufferToBuffer(encoder, wm->motion, 0, wm->prefix, 0,
                                             hdrg_mctf_motion_prefix(&wm->bme));
    }

    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(avg_group);

    for (k = 0; k <= work->count; k++) {
        wgpuBindGroupRelease(me_groups[k]);
    }

    ok = hdrg_gpu_scope_end(gpu);

    /* the next filter's job follows on the device's thread: nothing to read back */
    mapped = (ok && !device_out) ? hdrg_gpu_map_read(gpu, wm->readback, out_size) : NULL;

    if (!ok || (!device_out && mapped == NULL)) {
        for (k = 0; k < HDRG_WM_SLOTS; k++) {
            wm->frame_index[k] = UINT32_MAX;
            wm->pyramid_index[k] = UINT32_MAX;
        }

        return HDRG_ERR_INTERNAL;
    }

    if (!device_out) {
        memcpy(work->frames.out, mapped, out_size);
        wgpuBufferUnmap(wm->readback);
    }

    /* the motion to the window's frames */
    mapped = (work->count > 0u)
                 ? hdrg_gpu_map_read(gpu, wm->prefix, hdrg_mctf_motion_prefix(&wm->bme))
                 : NULL;

    if (mapped != NULL) {
        hdrg_mctf_motion_read(&wm->bme, (const uint8_t *)mapped, work->count, work->offsets,
                              wm->side);
        wgpuBufferUnmap(wm->prefix);
    }

    return (work->count == 0u || mapped != NULL) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

static void hdrg_wm_run(hdrg_gpu_t *gpu, void *arg)
{
    const hdrg_wm_t *wm = ((const hdrg_wm_work_t *)arg)->wm;
    hdrg_wm_work_t *work = (hdrg_wm_work_t *)arg;
    hdrg_status_t status;

    status = hdrg_wm_frame(gpu, work);

    /* the motion to the window's frames (a host without the side data takes the frame alone) */
    if (status == HDRG_OK && work->count > 0u) {
        status = work->api->job_set_side_data(
            work->job, HDRG_SD_MOTION, HDRG_SD_MOTION_VERSION, wm->side,
            hdrg_bme_motion_size(wm->bme.blocks_w[0], wm->bme.blocks_h[0], work->count));
        status = (status == HDRG_ERR_UNSUPPORTED) ? HDRG_OK : status;
    }

    (void)work->api->job_done(work->job, status);
    work->api->mem_free(work->api, work);
}

static void hdrg_wm_free(hdrg_wm_t *wm, const hdrg_plugin_api_t *api)
{
    if (wm->gpu != NULL) {
        (void)hdrg_gpu_call(wm->gpu, hdrg_wm_release, wm);
    }

    api->mem_free(api, wm->side);
    hdrg_gpu_destroy(wm->gpu);
    api->mem_free(api, wm);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wm_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wm_t *wm;
    hdrg_status_t status;
    int64_t past, future;
    double strength;

    status = api->option_get_int(options, HDRG_WM_LIT("past"), &past);
    if (status == HDRG_OK) {
        status = api->option_get_int(options, HDRG_WM_LIT("future"), &future);
    }

    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_WM_LIT("strength"), &strength);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_window(filter, (uint32_t)past, (uint32_t)future);
    }

    if (status != HDRG_OK) {
        return status;
    }

    wm = (hdrg_wm_t *)api->mem_calloc(api, 1, sizeof(*wm));
    if (wm == NULL) {
        return HDRG_ERR_NOMEM;
    }

    wm->past = (uint32_t)past;
    wm->future = (uint32_t)future;
    wm->strength = strength;
    status = api->filter_set_max_inflight(filter, 1);

    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &wm->gpu);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, HDRG_DEVICE_WEBGPU);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(wm->gpu, hdrg_wm_build, wm)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !wm->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_wm_free(wm, api);

        return status;
    }

    *inst = wm;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wm_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wm_free((hdrg_wm_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wm_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wm_t *wm = (hdrg_wm_t *)inst;
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
    if (!wm->fitted || width != wm->width || height != wm->height) {
        wm->fitted = false;

        if (!hdrg_wm_levels(wm, width, height, api) || !hdrg_gpu_call(wm->gpu, hdrg_wm_fit, wm) ||
            !wm->fitted) {
            return HDRG_ERR_NOMEM;
        }

        wm->width = width;
        wm->height = height;
    }

    /* the frame's noise: tau by level into the table, the blocks' bounds (hdrg.mctf's) */
    status = hdrg_mctf_noise(nego, api, wm->strength, wm->words + HDRG_MCTF_TAUS, &wm->block_lo,
                             &wm->block_hi, &wm->active);
    wm->active = wm->active && wm->past + wm->future > 0u;

    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, width, height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !wm->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wm_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wm_t *wm = (hdrg_wm_t *)inst;
    hdrg_wm_work_t *work;
    hdrg_status_t status;
    uint32_t index, past, future, begin, end, y, k, at;
    int32_t offset;
    size_t other_stride;
    void *frame;

    work = (hdrg_wm_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wm = wm;
    work->job = job;
    work->api = api;

    /* the job's frame and output: on the device (a chain the library runs there) or the host's */
    status = hdrg_webgpu_job_frames(wm->device, job, api, wm->width, wm->height, &work->frames);
    if (status == HDRG_OK) {
        status = api->job_window(job, &index, &past, &future);
    }

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    past = (past < wm->past) ? past : wm->past;
    future = (future < wm->future) ? future : wm->future;
    work->index[0] = index;
    work->rows[0] = work->frames.in;
    work->devices[0] =
        (work->frames.in_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.in_frame) : NULL;

    /* the frames of the window: nearer first, before then after (hdrg.mctf's order) */
    for (offset = 1; status == HDRG_OK && offset <= (int32_t)HDRG_WM_REACH; offset++) {
        for (k = 0; status == HDRG_OK && k < 2u; k++) {
            if ((k == 0u && (uint32_t)offset > past) || (k == 1u && (uint32_t)offset > future)) {
                continue;
            }

            at = work->count + 1u;

            /* the window on the device with the frame, else the host's frames */
            if (work->frames.in_frame != NULL) {
                status = api->job_device_window(job, (k == 0u) ? -offset : offset, &frame);
                if (status == HDRG_OK && frame == NULL) {
                    status = HDRG_ERR_INTERNAL;
                }

                work->devices[at] =
                    (status == HDRG_OK)
                        ? hdrg_gpu_frame_buffer((const hdrg_webgpu_device_frame_t *)frame)
                        : NULL;
            } else {
                status = api->job_input_frame(job, (k == 0u) ? -offset : offset, 0, &work->rows[at],
                                              &other_stride, &begin, &end);
                if (status == HDRG_OK && other_stride != work->frames.in_stride) {
                    status = HDRG_ERR_INTERNAL;
                }
            }

            work->index[at] = (k == 0u) ? index - (uint32_t)offset : index + (uint32_t)offset;
            work->offsets[work->count] = (k == 0u) ? -offset : offset;
            work->weight[work->count] = hdrg_wm_distance[offset];
            work->count++;
        }
    }

    if (status == HDRG_OK &&
        (work->frames.in_stride % 4u != 0u || work->frames.out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    /* none: the frame itself (the host's rows; on the device, the output alone) */
    if (work->count == 0u && work->frames.in_frame == NULL && work->frames.out_frame == NULL) {
        for (y = 0; y < wm->height; y++) {
            memcpy(work->frames.out + (size_t)y * work->frames.out_stride,
                   work->frames.in + (size_t)y * work->frames.in_stride, (size_t)wm->width * 6u);
        }

        api->mem_free(api, work);

        return api->job_done(job, HDRG_OK);
    }

    if (!hdrg_gpu_run(wm->gpu, hdrg_wm_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_mctf_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_WM_LIT(HDRG_WM_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = hdrg_mctf_options(cls, api);
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_DETERMINISTIC |
                                               HDRG_FILTER_ASYNC | HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_wm_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_wm_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_wm_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_wm_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_device_release(cls, hdrg_webgpu_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
