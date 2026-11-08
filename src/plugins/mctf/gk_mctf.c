/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "parts.h"
#include "mctf.h"

#define HDRG_LIT(s)   s, sizeof(s) - 1u
#define HDRG_GM_GROUP 8u /* the shader's workgroups: 8 x 8 */
#define HDRG_GM_REACH HDRG_MCTF_REACH
#define HDRG_GM_REFS  HDRG_MCTF_REFS
#define HDRG_GM_SLOTS HDRG_MCTF_SLOTS

/*
 * hdrg.mctf (src/plugins/mctf/mctf.c) on Metal and Vulkan (hdrgrading/gpu/filter_gk.h):
 * src/plugins/mctf/wgsl/mctf.wgsl through Tint, the CPU's integers and order:
 * the same output. The window's RGB16 frames are the plugin's host buffers
 * (the backend's frame_alloc), read by the GPU in place; the pyramids (one
 * a frame index, kept for the window) and the fields are the GPU's, the
 * table (the levels, the places, tau and the sample weights) a host buffer.
 * One pass a frame (the new frames' pyramids, each other frame's search and
 * its blocks' weights, the output), ended by its completion. ASYNC, a frame
 * in flight.
 */
typedef struct {
    const hdrg_gk_t *gk; /* first: the backend's frame_alloc's */
    hdrg_gk_kernel_t *lumas, *down, *coarse, *refine, *median, *blocks, *average;
    hdrg_gk_buffer_t *pyramid, *motion, *table, *readback; /* readback: the motion's prefix */
    hdrg_gk_device_t device;  /* its frames to the next filter, the others it read */
    hdrg_atomic_u32_t broken; /* a pass failed: no pyramid made before is to be trusted */
    hdrg_bme_t bme;           /* the levels (hdrg_bme_shape: no fields) */
    uint32_t *words;          /* the table's view */
    uint8_t *prefix;          /* the readback's view */
    uint8_t *side;            /* the output's HDRG_SD_MOTION */
    uint32_t pyramid_index[HDRG_GM_SLOTS]; /* the frames the slots' pyramids are of (UINT32_MAX) */
    uint32_t block_lo, block_hi;
    uint32_t width, height; /* configure's (0: none yet) */
    uint32_t past, future;
    bool active;
    double strength;
} hdrg_gm_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride, lv, slot_a, slot_b, ref_index, refs, shift,
        block_lo, block_hi, w[HDRG_GM_REFS];
} hdrg_gm_params_t;

/* a submitted job: what its end needs */
typedef struct {
    hdrg_gm_t *gm;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    hdrg_gpu_share_frame_t *in_frame, *out_frame; /* the job's on the device (NULL: the host's) */
    int32_t offsets[HDRG_GM_REFS];                /* the other frames' (the side data's fields) */
    uint32_t count;                               /* ... how many (0: no side data) */
} hdrg_gm_work_t;

/* the frames' weights by their distance from the job's (1/256, hdrg.mctf's) */
static const uint32_t hdrg_gm_distance[HDRG_GM_REACH + 1u] = {256u, 218u, 146u};

static uint32_t hdrg_gm_div(uint32_t v, uint32_t d)
{
    return (v + d - 1u) / d;
}

/*
 * a pass's end: its output frame on the device marked (its job ended at the commit), or the
 * job's; failed with the pass or its input
 */
static void hdrg_gm_done(void *arg, bool ok)
{
    hdrg_gm_work_t *work = (hdrg_gm_work_t *)arg;
    hdrg_status_t status;

    ok = ok &&
         (work->in_frame == NULL || hdrg_atomic_u32_load_acquire(&work->in_frame->failed) == 0u);

    if (!ok) {
        hdrg_atomic_u32_store_release(&work->gm->broken, 1u);
    }

    if (work->out_frame != NULL) {
        hdrg_atomic_u32_store_release(&work->out_frame->failed, ok ? 0u : 1u);
    }

    hdrg_gpu_share_drop(work->in_frame, work->out_frame);

    /* the job ends here unless it did at the commit (its frame on the device, no side data) */
    if (work->out_frame == NULL || work->count > 0u) {
        status = ok ? HDRG_OK : HDRG_ERR_INTERNAL;

        if (status == HDRG_OK && work->count > 0u) {
            hdrg_mctf_motion_read(&work->gm->bme, work->gm->prefix, work->count, work->offsets,
                                  work->gm->side);
            status = work->api->job_set_side_data(
                work->job, HDRG_SD_MOTION, HDRG_SD_MOTION_VERSION, work->gm->side,
                hdrg_bme_motion_size(work->gm->bme.blocks_w[0], work->gm->bme.blocks_h[0],
                                     work->count));
            status = (status == HDRG_ERR_UNSUPPORTED) ? HDRG_OK : status;
        }

        (void)work->api->job_done(work->job, status);
    }

    work->api->mem_free(work->api, work);
}

static void hdrg_gm_buffers_free(hdrg_gm_t *gm, const hdrg_plugin_api_t *api)
{
    hdrg_gk_buffer_free(gm->gk, gm->pyramid);
    hdrg_gk_buffer_free(gm->gk, gm->motion);
    hdrg_gk_buffer_free(gm->gk, gm->table);
    hdrg_gk_buffer_free(gm->gk, gm->readback);
    api->mem_free(api, gm->side);
    gm->pyramid = NULL;
    gm->motion = NULL;
    gm->table = NULL;
    gm->readback = NULL;
    gm->words = NULL;
    gm->prefix = NULL;
    gm->side = NULL;
    gm->bme.levels = 0;
    gm->width = 0;
    gm->height = 0;
}

static void hdrg_gm_release(hdrg_gm_t *gm, const hdrg_plugin_api_t *api)
{
    hdrg_gk_device_frames_free(gm->gk, &gm->device);

    hdrg_gk_kernel_free(gm->gk, gm->lumas);
    hdrg_gk_kernel_free(gm->gk, gm->down);
    hdrg_gk_kernel_free(gm->gk, gm->coarse);
    hdrg_gk_kernel_free(gm->gk, gm->refine);
    hdrg_gk_kernel_free(gm->gk, gm->median);
    hdrg_gk_kernel_free(gm->gk, gm->blocks);
    hdrg_gk_kernel_free(gm->gk, gm->average);
    hdrg_gm_buffers_free(gm, api);
    hdrg_gk_plugin_release(api, gm->gk);
    api->mem_free(api, gm);
}

/* the levels of hdrg_bme_shape for width x height, the buffers and the table (hdrg_mctf_table) */
static bool hdrg_gm_fit(hdrg_gm_t *gm, uint32_t width, uint32_t height,
                        const hdrg_plugin_api_t *api)
{
    uint64_t pyramid, motion;
    uint32_t k;
    uint8_t *view;

    if (gm->width == width && gm->height == height) {
        return true;
    }

    hdrg_gm_buffers_free(gm, api);
    hdrg_bme_shape(&gm->bme, width, height);
    gm->table = hdrg_gk_host_buffer(gm->gk, HDRG_MCTF_WORDS * sizeof(uint32_t), &view);
    if (gm->table == NULL) {
        return false;
    }

    gm->words = (uint32_t *)(void *)view;

    if (!hdrg_mctf_table(&gm->bme, gm->words, &pyramid, &motion)) {
        return false;
    }

    gm->pyramid = hdrg_gk_buffer(gm->gk, (size_t)pyramid * 4u);
    gm->motion = hdrg_gk_buffer(gm->gk, (size_t)motion * 4u);
    gm->readback = hdrg_gk_host_buffer(gm->gk, hdrg_mctf_motion_prefix(&gm->bme), &gm->prefix);
    gm->side = (uint8_t *)api->mem_alloc(
        api, hdrg_bme_motion_size(gm->bme.blocks_w[0], gm->bme.blocks_h[0], HDRG_GM_REFS));

    if (gm->pyramid == NULL || gm->motion == NULL || gm->readback == NULL || gm->side == NULL) {
        return false;
    }

    for (k = 0; k < HDRG_GM_SLOTS; k++) {
        gm->pyramid_index[k] = UINT32_MAX;
    }

    gm->width = width;
    gm->height = height;

    return true;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_gm_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const lumas[3] = {"src", "pyramid", "table"}, *const down[2] = {"pyramid", "table"},
                      *const search[3] = {"pyramid", "motion", "table"},
                      *const median[2] = {"motion", "table"},
                      *const average[8] = {"motion", "table", "f0", "f1", "f2", "f3", "f4", "dst"};
    hdrg_gm_t *gm;
    hdrg_status_t status;
    int64_t past, future;
    double strength;

    status = api->option_get_int(options, HDRG_LIT("past"), &past);
    if (status == HDRG_OK) {
        status = api->option_get_int(options, HDRG_LIT("future"), &future);
    }

    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_LIT("strength"), &strength);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_window(filter, (uint32_t)past, (uint32_t)future);
    }

    if (status != HDRG_OK) {
        return status;
    }

    gm = (hdrg_gm_t *)api->mem_calloc(api, 1, sizeof(*gm));
    if (gm == NULL) {
        return HDRG_ERR_NOMEM;
    }

    gm->past = (uint32_t)past;
    gm->future = (uint32_t)future;
    gm->strength = strength;
    hdrg_atomic_u32_init(&gm->broken, 0u);

    status = api->filter_set_max_inflight(filter, 1);
    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &gm->gk);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, gm->gk->ops->kind);
    }

    if (status == HDRG_OK) {
        gm->lumas = hdrg_gk_kernel(gm->gk, &hdrg_gk_mctf_lumas, "params", lumas, 3);
        gm->down = hdrg_gk_kernel(gm->gk, &hdrg_gk_mctf_down, "params", down, 2);
        gm->coarse = hdrg_gk_kernel(gm->gk, &hdrg_gk_mctf_coarse, "params", search, 3);
        gm->refine = hdrg_gk_kernel(gm->gk, &hdrg_gk_mctf_refine, "params", search, 3);
        gm->median = hdrg_gk_kernel(gm->gk, &hdrg_gk_mctf_median, "params", median, 2);
        gm->blocks = hdrg_gk_kernel(gm->gk, &hdrg_gk_mctf_blocks, "params", search, 3);
        gm->average = hdrg_gk_kernel(gm->gk, &hdrg_gk_mctf_average, "params", average, 8);
        status =
            (gm->lumas != NULL && gm->down != NULL && gm->coarse != NULL && gm->refine != NULL &&
             gm->median != NULL && gm->blocks != NULL && gm->average != NULL)
                ? HDRG_OK
                : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_gm_release(gm, api);

        return status;
    }

    *inst = gm;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_gm_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_gm_release((hdrg_gm_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_gm_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_gm_t *gm = (hdrg_gm_t *)inst;
    hdrg_status_t status;
    uint32_t width, height, format;

    status = api->nego_input(nego, &width, &height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || width == 0 || height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    if (!hdrg_gm_fit(gm, width, height, api)) {
        hdrg_gm_buffers_free(gm, api);

        return HDRG_ERR_NOMEM;
    }

    /* the frame's noise: tau by level into the table, the blocks' bounds (hdrg.mctf's) */
    status = hdrg_mctf_noise(nego, api, gm->strength, gm->words + HDRG_MCTF_TAUS, &gm->block_lo,
                             &gm->block_hi, &gm->active);
    gm->active = gm->active && gm->past + gm->future > 0u;

    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, width, height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !gm->active) ? api->nego_set_passthrough(nego) : status;
}

/* the pyramid of frame index (handle) into its slot in the pass, unless kept */
static void hdrg_gm_pyramid(hdrg_gm_t *gm, hdrg_gk_pass_t *pass, hdrg_gm_params_t *params,
                            uint32_t index, void *handle)
{
    const uint32_t slot = index % HDRG_GM_SLOTS;
    hdrg_gk_buffer_t *buffers[3];
    uint32_t level;

    if (gm->pyramid_index[slot] == index) {
        return;
    }

    gm->pyramid_index[slot] = index;
    params->slot_a = slot;
    params->lv = 0;
    buffers[0] = (hdrg_gk_buffer_t *)handle;
    buffers[1] = gm->pyramid;
    buffers[2] = gm->table;
    hdrg_gk_dispatch(gm->gk, pass, gm->lumas, params, sizeof(*params), buffers,
                     hdrg_gm_div(gm->bme.sizes[0][0], HDRG_GM_GROUP),
                     hdrg_gm_div(gm->bme.sizes[0][1], HDRG_GM_GROUP), HDRG_GM_GROUP, HDRG_GM_GROUP);
    buffers[0] = gm->pyramid;
    buffers[1] = gm->table;

    for (level = 1; level < gm->bme.levels; level++) {
        params->lv = level;
        hdrg_gk_dispatch(gm->gk, pass, gm->down, params, sizeof(*params), buffers,
                         hdrg_gm_div(gm->bme.sizes[level][0], HDRG_GM_GROUP),
                         hdrg_gm_div(gm->bme.sizes[level][1], HDRG_GM_GROUP), HDRG_GM_GROUP,
                         HDRG_GM_GROUP);
    }
}

/* the search of other frame k (of slot) and its blocks' weights in the pass */
static void hdrg_gm_search(hdrg_gm_t *gm, hdrg_gk_pass_t *pass, hdrg_gm_params_t *params,
                           uint32_t own, uint32_t slot, uint32_t k)
{
    hdrg_gk_buffer_t *buffers[3];
    uint32_t level;

    params->slot_a = own;
    params->slot_b = slot;
    params->ref_index = k;
    buffers[0] = gm->pyramid;
    buffers[1] = gm->motion;
    buffers[2] = gm->table;

    /* coarse to fine, each level's field smoothed by its median */
    for (level = gm->bme.levels; level-- > 0;) {
        params->lv = level;
        hdrg_gk_dispatch(
            gm->gk, pass, (level + 1u == gm->bme.levels) ? gm->coarse : gm->refine, params,
            sizeof(*params), buffers, hdrg_gm_div(gm->bme.blocks_w[level], HDRG_GM_GROUP),
            hdrg_gm_div(gm->bme.blocks_h[level], HDRG_GM_GROUP), HDRG_GM_GROUP, HDRG_GM_GROUP);
        hdrg_gk_dispatch(gm->gk, pass, gm->median, params, sizeof(*params), buffers + 1,
                         hdrg_gm_div(gm->bme.blocks_w[level], HDRG_GM_GROUP),
                         hdrg_gm_div(gm->bme.blocks_h[level], HDRG_GM_GROUP), HDRG_GM_GROUP,
                         HDRG_GM_GROUP);
    }

    params->lv = 0;
    hdrg_gk_dispatch(gm->gk, pass, gm->blocks, params, sizeof(*params), buffers,
                     hdrg_gm_div(gm->bme.blocks_w[0], HDRG_GM_GROUP),
                     hdrg_gm_div(gm->bme.blocks_h[0], HDRG_GM_GROUP), HDRG_GM_GROUP, HDRG_GM_GROUP);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_gm_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const uint8_t *other;
    hdrg_gm_t *gm = (hdrg_gm_t *)inst;
    hdrg_gk_buffer_t *buffers[8], *handles[HDRG_GM_REFS];
    hdrg_gk_job_t frames;
    hdrg_gm_params_t params;
    hdrg_gm_work_t *work;
    hdrg_gk_pass_t *pass;
    hdrg_status_t status;
    uint32_t index, past, future, begin, end, count, expected, k, slots[HDRG_GM_REFS],
        indices[HDRG_GM_REFS];
    int32_t offset, offsets[HDRG_GM_REFS];
    size_t other_stride;
    void *handle;

    /* the job's frame and output: on the device (a chain the library runs there) or the host's */
    status = hdrg_gk_job_frames(gm->gk, &gm->device, job, api, gm->width, gm->height, 6u, &frames);
    if (status == HDRG_OK) {
        status = api->job_window(job, &index, &past, &future);
    }

    if (status != HDRG_OK) {
        return status;
    }

    memset(&params, 0, sizeof(params));
    past = (past < gm->past) ? past : gm->past;
    future = (future < gm->future) ? future : gm->future;
    count = 0;

    /* the frames of the window: nearer first, before then after (hdrg.mctf's order) */
    for (offset = 1; status == HDRG_OK && offset <= (int32_t)HDRG_GM_REACH; offset++) {
        for (k = 0; status == HDRG_OK && k < 2u; k++) {
            if ((k == 0u && (uint32_t)offset > past) || (k == 1u && (uint32_t)offset > future)) {
                continue;
            }

            handle = NULL;

            /* the window on the device with the frame, else the host's frames */
            if (frames.in_frame != NULL) {
                status = api->job_device_window(job, (k == 0u) ? -offset : offset, &handle);
                if (status == HDRG_OK && handle == NULL) {
                    status = HDRG_ERR_INTERNAL;
                }

                handles[count] = (status == HDRG_OK)
                                     ? hdrg_gk_wrap(gm->gk, &gm->device.read,
                                                    (const hdrg_gpu_share_frame_t *)handle)
                                     : NULL;

                if (status == HDRG_OK && handles[count] == NULL) {
                    status = HDRG_ERR_INTERNAL;
                }
            } else {
                status = api->job_input_frame(job, (k == 0u) ? -offset : offset, 0, &other,
                                              &other_stride, &begin, &end);
                if (status == HDRG_OK) {
                    status = api->job_input_handle(job, (k == 0u) ? -offset : offset, &handle);
                }

                if (status == HDRG_OK && (other_stride != frames.in_stride || handle == NULL)) {
                    status = HDRG_ERR_INTERNAL;
                }

                handles[count] = (hdrg_gk_buffer_t *)handle;
            }

            indices[count] = (k == 0u) ? index - (uint32_t)offset : index + (uint32_t)offset;
            offsets[count] = (k == 0u) ? -offset : offset;
            params.w[count] = hdrg_gm_distance[offset];
            count++;
        }
    }

    if (status == HDRG_OK && (frames.in == NULL || frames.out == NULL || gm->width == 0)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        return status;
    }

    /* a pass failed: no pyramid made before it is to be trusted */
    expected = 1u;

    if (hdrg_atomic_u32_cas_acq_rel(&gm->broken, &expected, 0u)) {
        for (k = 0; k < HDRG_GM_SLOTS; k++) {
            gm->pyramid_index[k] = UINT32_MAX;
        }
    }

    /* none: the frame itself (the host's rows; on the device, the output alone) */
    if (count == 0u && frames.in_frame == NULL && frames.out_frame == NULL) {
        return hdrg_gk_job_pass(job, api, &frames, gm->width, gm->height, 6u);
    }

    work = (hdrg_gm_work_t *)api->mem_alloc(api, sizeof(*work));

    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->gm = gm;
    work->job = job;
    work->api = api;
    work->in_frame = frames.in_frame;
    work->out_frame = (frames.out_frame != NULL) ? &frames.out_frame->share : NULL;
    work->count = count;
    memcpy(work->offsets, offsets, sizeof(offsets));

    pass = hdrg_gk_begin(gm->gk);
    if (pass == NULL) {
        api->mem_free(api, work);

        return HDRG_ERR_INTERNAL;
    }

    params.width = gm->width;
    params.height = gm->height;
    params.in_stride = (uint32_t)(frames.in_stride / 4u);
    params.out_stride = (uint32_t)(frames.out_stride / 4u);
    params.refs = count;
    params.shift = HDRG_MCTF_SHIFT;
    params.block_lo = gm->block_lo;
    params.block_hi = gm->block_hi;

    if (count > 0u) {
        hdrg_gm_pyramid(gm, pass, &params, index, frames.in);
    }

    for (k = 0; k < count; k++) {
        hdrg_gm_pyramid(gm, pass, &params, indices[k], handles[k]);
        slots[k] = indices[k] % HDRG_GM_SLOTS;
    }

    for (k = 0; k < count; k++) {
        hdrg_gm_search(gm, pass, &params, index % HDRG_GM_SLOTS, slots[k], k);
    }

    /* the output: the job's frame and the others (the job's again for the bindings not read) */
    params.lv = 0;
    buffers[0] = gm->motion;
    buffers[1] = gm->table;
    buffers[2] = frames.in;

    for (k = 0; k < HDRG_GM_REFS; k++) {
        buffers[3u + k] = (k < count) ? handles[k] : frames.in;
    }

    buffers[7] = frames.out;
    hdrg_gk_dispatch(gm->gk, pass, gm->average, &params, sizeof(params), buffers,
                     hdrg_gm_div(hdrg_gm_div(gm->width, 2u), HDRG_GM_GROUP),
                     hdrg_gm_div(gm->height, HDRG_GM_GROUP), HDRG_GM_GROUP, HDRG_GM_GROUP);

    /* the side data's part of the motion, read at the end */
    if (count > 0u) {
        hdrg_gk_copy_buffer(gm->gk, pass, gm->motion, gm->readback,
                            hdrg_mctf_motion_prefix(&gm->bme));
    }

    hdrg_gpu_share_hold(work->in_frame, work->out_frame, 1u);

    if (!hdrg_gk_commit(gm->gk, pass, hdrg_gm_done, work)) {
        for (k = 0; k < HDRG_GM_SLOTS; k++) {
            gm->pyramid_index[k] = UINT32_MAX;
        }

        hdrg_gpu_share_drop(work->in_frame, work->out_frame);
        api->mem_free(api, work);

        return HDRG_ERR_INTERNAL;
    }

    /* the next filter's passes follow on the queue: the job is through for the host */
    if (frames.out_frame != NULL && count == 0u) {
        (void)api->job_done(job, HDRG_OK);
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_gk_mctf_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                           const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "mctf");
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
        status = api->class_set_create(cls, hdrg_gm_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_gm_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_gm_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_gm_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_device_release(cls, hdrg_gk_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_set_frames(cls, backend->frame_alloc, backend->frame_free);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
