/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/plugins/blockme.h"
#include "parts.h"

#define HDRG_GI_GROUP   8u  /* the shader's workgroups: 8 x 8 */
#define HDRG_GI_TABLE   8u  /* the shader's table: words a level ... */
#define HDRG_GI_SCRATCH 48u /* ... and the scratch's place */

/*
 * hdrg.interp.mc (src/plugins/interp/interp_mc.c) on Metal and Vulkan
 * (hdrgrading/gpu/filter_gk.h): src/plugins/interp/wgsl/interp.wgsl through Tint
 * (src/plugins/interp/msl/interp_*, src/plugins/interp/spirv/interp_*), the CPU's levels,
 * integers and order: the same output. The jobs' RGB16 frames (the frame,
 * the next one, the output) are the plugin's host buffers (the backend's
 * frame_alloc), read and written by the GPU in place; the pyramid and the
 * fields are the GPU's, the levels' table a host buffer. One pass a frame
 * (the luma, the pyramid, a search and a median each level coarse to
 * fine, the output, the finest field's records), ended by its completion:
 * the output's HDRG_SD_MOTION from the records (a host buffer) before
 * job_done. ASYNC, a frame in flight.
 */
typedef struct {
    const hdrg_gk_t *gk; /* first: the backend's frame_alloc's */
    hdrg_gk_kernel_t *lumas, *down, *coarse, *refine, *median, *compose, *confidence;
    hdrg_gk_buffer_t *pyramid, *fields, *table, *records;
    const uint8_t *records_view; /* the records' words, read after the pass */
    uint8_t *side;               /* the output's HDRG_SD_MOTION (side_size bytes) */
    size_t side_size;
    uint32_t width, height, levels; /* configure's (levels 0: none yet) */
    uint32_t sizes[HDRG_BME_LEVELS][2], blocks[HDRG_BME_LEVELS][2];
} hdrg_gi_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, a_stride, b_stride, out_stride, phase, lv, levels;
} hdrg_gi_params_t;

/* a submitted job: what its end needs */
typedef struct {
    hdrg_gi_t *gi;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    uint32_t phase;
} hdrg_gi_work_t;

static uint32_t hdrg_gi_div(uint32_t v, uint32_t d)
{
    return (v + d - 1u) / d;
}

/* the output's HDRG_SD_MOTION from the pass's records into the job */
static hdrg_status_t hdrg_gi_motion(const hdrg_gi_t *gi, hdrg_job_t *job,
                                    const hdrg_plugin_api_t *api, uint32_t phase)
{
    hdrg_status_t status;

    hdrg_interp_motion_head(gi->blocks[0][0], gi->blocks[0][1], phase, gi->side);
    hdrg_interp_motion_records(gi->records_view, gi->blocks[0][0], gi->blocks[0][1], gi->side);

    /* a host without the output's side data (none asked for) takes the frame alone */
    status = api->job_set_side_data(job, HDRG_SD_MOTION, HDRG_SD_MOTION_VERSION, gi->side,
                                    gi->side_size);

    return (status == HDRG_ERR_UNSUPPORTED) ? HDRG_OK : status;
}

static void hdrg_gi_done(void *arg, bool ok)
{
    hdrg_gi_work_t *work = (hdrg_gi_work_t *)arg;
    hdrg_status_t status;

    status = ok ? hdrg_gi_motion(work->gi, work->job, work->api, work->phase) : HDRG_ERR_INTERNAL;

    (void)work->api->job_done(work->job, status);
    work->api->mem_free(work->api, work);
}

static void hdrg_gi_buffers_free(hdrg_gi_t *gi, const hdrg_plugin_api_t *api)
{
    hdrg_gk_buffer_free(gi->gk, gi->pyramid);
    hdrg_gk_buffer_free(gi->gk, gi->fields);
    hdrg_gk_buffer_free(gi->gk, gi->table);
    hdrg_gk_buffer_free(gi->gk, gi->records);
    api->mem_free(api, gi->side);
    gi->pyramid = NULL;
    gi->fields = NULL;
    gi->table = NULL;
    gi->records = NULL;
    gi->records_view = NULL;
    gi->side = NULL;
    gi->levels = 0;
}

static void hdrg_gi_release(hdrg_gi_t *gi, const hdrg_plugin_api_t *api)
{
    hdrg_gk_kernel_free(gi->gk, gi->lumas);
    hdrg_gk_kernel_free(gi->gk, gi->down);
    hdrg_gk_kernel_free(gi->gk, gi->coarse);
    hdrg_gk_kernel_free(gi->gk, gi->refine);
    hdrg_gk_kernel_free(gi->gk, gi->median);
    hdrg_gk_kernel_free(gi->gk, gi->compose);
    hdrg_gk_kernel_free(gi->gk, gi->confidence);
    hdrg_gi_buffers_free(gi, api);
    hdrg_gk_plugin_release(api, gi->gk);
    api->mem_free(api, gi);
}

/* the levels of hdrg_mc_fit for width x height, the buffers and the table (kept while the size
 * stays) */
static bool hdrg_gi_fit(hdrg_gi_t *gi, uint32_t width, uint32_t height,
                        const hdrg_plugin_api_t *api)
{
    hdrg_bme_t shape;
    uint64_t pyramid, fields;
    uint32_t count, level, *table, *row;
    uint8_t *view;
    size_t blocks;

    if (gi->levels != 0 && gi->width == width && gi->height == height) {
        return true;
    }

    hdrg_gi_buffers_free(gi, api);
    hdrg_bme_shape(&shape, width, height);
    count = shape.levels;

    for (level = 0; level < count; level++) {
        gi->sizes[level][0] = shape.sizes[level][0];
        gi->sizes[level][1] = shape.sizes[level][1];
        gi->blocks[level][0] = shape.blocks_w[level];
        gi->blocks[level][1] = shape.blocks_h[level];
    }

    gi->table = hdrg_gk_host_buffer(gi->gk, (HDRG_GI_SCRATCH + 1u) * sizeof(uint32_t), &view);
    if (gi->table == NULL) {
        return false;
    }

    table = (uint32_t *)(void *)view;
    memset(table, 0, (HDRG_GI_SCRATCH + 1u) * sizeof(uint32_t));
    pyramid = 0;
    fields = 0;

    /* each level's size, its planes (the first frame's, the second's), its blocks, its field */
    for (level = 0; level < count; level++) {
        row = table + level * HDRG_GI_TABLE;
        row[0] = gi->sizes[level][0];
        row[1] = gi->sizes[level][1];
        row[2] = (uint32_t)pyramid;
        row[3] = (uint32_t)(pyramid + (uint64_t)row[0] * row[1]);
        row[4] = gi->blocks[level][0];
        row[5] = gi->blocks[level][1];
        row[6] = (uint32_t)fields;
        pyramid += 2u * (uint64_t)row[0] * row[1];
        fields += 2u * (uint64_t)row[4] * row[5];
    }

    table[HDRG_GI_SCRATCH] = (uint32_t)fields;
    fields += 2u * (uint64_t)gi->blocks[0][0] * gi->blocks[0][1];

    if (pyramid > UINT32_MAX || fields > UINT32_MAX || pyramid * 4u > (uint64_t)SIZE_MAX ||
        fields * 4u > (uint64_t)SIZE_MAX) {
        return false;
    }

    blocks = (size_t)gi->blocks[0][0] * gi->blocks[0][1];
    gi->side_size = hdrg_bme_motion_size(gi->blocks[0][0], gi->blocks[0][1], 1u);
    gi->pyramid = hdrg_gk_buffer(gi->gk, (size_t)pyramid * 4u);
    gi->fields = hdrg_gk_buffer(gi->gk, (size_t)fields * 4u);
    gi->records = hdrg_gk_host_buffer(gi->gk, blocks * 2u * sizeof(uint32_t), &view);
    gi->records_view = view;
    gi->side = (uint8_t *)api->mem_alloc(api, gi->side_size);
    if (gi->pyramid == NULL || gi->fields == NULL || gi->records == NULL || gi->side == NULL) {
        return false;
    }

    gi->levels = count;
    gi->width = width;
    gi->height = height;

    return true;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_gi_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const lumas[4] = {"frame_a", "frame_b", "pyramid", "table"},
                      *const down[2] = {"pyramid", "table"},
                      *const search[3] = {"pyramid", "fields", "table"},
                      *const median[2] = {"fields", "table"},
                      *const compose[5] = {"frame_a", "frame_b", "fields", "table", "dst"},
                      *const confidence[4] = {"pyramid", "fields", "table", "records"};
    hdrg_gi_t *gi;
    hdrg_status_t status;

    (void)options;

    gi = (hdrg_gi_t *)api->mem_calloc(api, 1, sizeof(*gi));
    if (gi == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->filter_set_max_inflight(filter, 1);
    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &gi->gk);
    }

    if (status == HDRG_OK) {
        gi->lumas = hdrg_gk_kernel(gi->gk, &hdrg_gk_interp_lumas, "params", lumas, 4);
        gi->down = hdrg_gk_kernel(gi->gk, &hdrg_gk_interp_down, "params", down, 2);
        gi->coarse = hdrg_gk_kernel(gi->gk, &hdrg_gk_interp_coarse, "params", search, 3);
        gi->refine = hdrg_gk_kernel(gi->gk, &hdrg_gk_interp_refine, "params", search, 3);
        gi->median = hdrg_gk_kernel(gi->gk, &hdrg_gk_interp_median, "params", median, 2);
        gi->compose = hdrg_gk_kernel(gi->gk, &hdrg_gk_interp_compose, "params", compose, 5);
        gi->confidence =
            hdrg_gk_kernel(gi->gk, &hdrg_gk_interp_confidence, "params", confidence, 4);
        status =
            (gi->lumas != NULL && gi->down != NULL && gi->coarse != NULL && gi->refine != NULL &&
             gi->median != NULL && gi->compose != NULL && gi->confidence != NULL)
                ? HDRG_OK
                : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_gi_release(gi, api);

        return status;
    }

    *inst = gi;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_gi_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_gi_release((hdrg_gi_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_gi_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_gi_t *gi = (hdrg_gi_t *)inst;
    hdrg_status_t status;
    uint32_t width, height, format;

    status = api->nego_input(nego, &width, &height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || width == 0 || height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    if (!hdrg_gi_fit(gi, width, height, api)) {
        hdrg_gi_buffers_free(gi, api);

        return HDRG_ERR_NOMEM;
    }

    return api->nego_set_output(nego, width, height, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_gi_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const uint8_t *a_rows, *b_rows;
    hdrg_gi_t *gi = (hdrg_gi_t *)inst;
    hdrg_gk_buffer_t *buffers[5];
    hdrg_gi_params_t params;
    hdrg_gi_work_t *work;
    hdrg_gk_pass_t *pass;
    hdrg_status_t status;
    uint32_t begin, end, level, phase;
    uint8_t *out;
    size_t a_stride, b_stride, out_stride;
    void *a, *b, *dst;

    status = api->job_input(job, 0, &a_rows, &a_stride, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_input_frame(job, 1, 0, &b_rows, &b_stride, &begin, &end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &out, &out_stride);
    }

    if (status == HDRG_OK) {
        status = api->job_input_handle(job, 0, &a);
    }

    if (status == HDRG_OK) {
        status = api->job_input_handle(job, 1, &b);
    }

    if (status == HDRG_OK) {
        status = api->job_output_handle(job, &dst);
    }

    if (status == HDRG_OK) {
        status = api->job_phase(job, &phase);
    }

    if (status == HDRG_OK &&
        (gi->levels == 0 || a == NULL || b == NULL || dst == NULL || a_stride % 4u != 0 ||
         b_stride % 4u != 0 || out_stride % 4u != 0 || phase > 65536u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        return status;
    }

    work = (hdrg_gi_work_t *)api->mem_alloc(api, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->gi = gi;
    work->job = job;
    work->api = api;
    work->phase = phase;

    pass = hdrg_gk_begin(gi->gk);
    if (pass == NULL) {
        api->mem_free(api, work);

        return HDRG_ERR_INTERNAL;
    }

    memset(&params, 0, sizeof(params));
    params.width = gi->width;
    params.height = gi->height;
    params.a_stride = (uint32_t)(a_stride / 4u);
    params.b_stride = (uint32_t)(b_stride / 4u);
    params.out_stride = (uint32_t)(out_stride / 4u);
    params.phase = phase;
    params.levels = gi->levels;

    /* the luma of both frames, then the pyramid */
    buffers[0] = (hdrg_gk_buffer_t *)a;
    buffers[1] = (hdrg_gk_buffer_t *)b;
    buffers[2] = gi->pyramid;
    buffers[3] = gi->table;
    hdrg_gk_dispatch(gi->gk, pass, gi->lumas, &params, sizeof(params), buffers,
                     hdrg_gi_div(gi->sizes[0][0], HDRG_GI_GROUP),
                     hdrg_gi_div(gi->sizes[0][1], HDRG_GI_GROUP), HDRG_GI_GROUP, HDRG_GI_GROUP);
    buffers[0] = gi->pyramid;
    buffers[1] = gi->table;

    for (level = 1; level < gi->levels; level++) {
        params.lv = level;
        hdrg_gk_dispatch(gi->gk, pass, gi->down, &params, sizeof(params), buffers,
                         hdrg_gi_div(gi->sizes[level][0], HDRG_GI_GROUP),
                         hdrg_gi_div(gi->sizes[level][1], HDRG_GI_GROUP), HDRG_GI_GROUP,
                         HDRG_GI_GROUP);
    }

    /* coarse to fine, each level's field smoothed by its median */
    for (level = gi->levels; level-- > 0;) {
        params.lv = level;
        buffers[0] = gi->pyramid;
        buffers[1] = gi->fields;
        buffers[2] = gi->table;
        hdrg_gk_dispatch(
            gi->gk, pass, (level + 1u == gi->levels) ? gi->coarse : gi->refine, &params,
            sizeof(params), buffers, hdrg_gi_div(gi->blocks[level][0], HDRG_GI_GROUP),
            hdrg_gi_div(gi->blocks[level][1], HDRG_GI_GROUP), HDRG_GI_GROUP, HDRG_GI_GROUP);
        hdrg_gk_dispatch(gi->gk, pass, gi->median, &params, sizeof(params), buffers + 1,
                         hdrg_gi_div(gi->blocks[level][0], HDRG_GI_GROUP),
                         hdrg_gi_div(gi->blocks[level][1], HDRG_GI_GROUP), HDRG_GI_GROUP,
                         HDRG_GI_GROUP);
    }

    params.lv = 0;
    buffers[0] = (hdrg_gk_buffer_t *)a;
    buffers[1] = (hdrg_gk_buffer_t *)b;
    buffers[2] = gi->fields;
    buffers[3] = gi->table;
    buffers[4] = (hdrg_gk_buffer_t *)dst;
    hdrg_gk_dispatch(gi->gk, pass, gi->compose, &params, sizeof(params), buffers,
                     hdrg_gi_div(hdrg_gi_div(gi->width, 2u), HDRG_GI_GROUP),
                     hdrg_gi_div(gi->height, HDRG_GI_GROUP), HDRG_GI_GROUP, HDRG_GI_GROUP);
    buffers[0] = gi->pyramid;
    buffers[1] = gi->fields;
    buffers[2] = gi->table;
    buffers[3] = gi->records;
    hdrg_gk_dispatch(gi->gk, pass, gi->confidence, &params, sizeof(params), buffers,
                     hdrg_gi_div(gi->blocks[0][0], HDRG_GI_GROUP),
                     hdrg_gi_div(gi->blocks[0][1], HDRG_GI_GROUP), HDRG_GI_GROUP, HDRG_GI_GROUP);

    if (!hdrg_gk_commit(gi->gk, pass, hdrg_gi_done, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_INTERNAL;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_gk_interp_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                             const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "interp.mc");
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_INTERP);
    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_DETERMINISTIC |
                                               HDRG_FILTER_ASYNC | HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_gi_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_gi_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_gi_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_gi_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_frames(cls, backend->frame_alloc, backend->frame_free);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
