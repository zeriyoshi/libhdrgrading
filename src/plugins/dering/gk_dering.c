/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)      s, sizeof(s) - 1u
#define HDRG_MR_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */
#define HDRG_MR_GROUP    8u /* the shader's workgroups: 8 x 8 */
#define HDRG_MR_GRID     8u /* the blocks' grid */

/*
 * hdrg.dering (src/plugins/dering/dering.c) on Metal and Vulkan
 * (hdrgrading/gpu/filter_gk.h): the WGSL of hdrg.webgpu.dering
 * (src/plugins/dering/wgsl/dering.wgsl) through Tint (src/plugins/dering/msl/dering_*,
 * src/plugins/dering/spirv/dering_*), the same settings, blocks, ranges, windows and
 * passthrough; the output equals the CPU's. The jobs' RGB16 frames are the
 * plugin's host buffers (the backend's frame_alloc), read and written by the GPU
 * in place; two dispatches a frame, ended by the pass's completion.
 * ASYNC, HDRG_MR_INFLIGHT frames in flight, each with a block buffer of its
 * own.
 */
typedef struct {
    const hdrg_gk_t *gk;     /* first: the backend's frame_alloc's */
    hdrg_gk_device_t device; /* its frames to the next filter, the others it read */
    hdrg_gk_kernel_t *contrasts, *dering;
    hdrg_gk_buffer_t *contrast[HDRG_MR_INFLIGHT];
    size_t blocks;                 /* the block buffers' (0: none yet) */
    uint32_t width, height, reach; /* configure's */
    uint32_t slot;                 /* the next job's block buffer */
    int32_t floor;                 /* configure's: hdrg.dering's settings */
    bool active;
    double strength;
} hdrg_mr_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride, blocks, reach;
    int32_t floor;
    uint32_t pad;
} hdrg_mr_params_t;

static void hdrg_mr_release(hdrg_mr_t *mr, const hdrg_plugin_api_t *api)
{
    uint8_t i;

    hdrg_gk_kernel_free(mr->gk, mr->contrasts);
    hdrg_gk_kernel_free(mr->gk, mr->dering);

    for (i = 0; i < HDRG_MR_INFLIGHT; i++) {
        hdrg_gk_buffer_free(mr->gk, mr->contrast[i]);
    }

    hdrg_gk_device_frames_free(mr->gk, &mr->device);
    hdrg_gk_plugin_release(api, mr->gk);
    api->mem_free(api, mr);
}

/* the block buffers of every slot for blocks, kept while they are large enough */
static bool hdrg_mr_buffers(hdrg_mr_t *mr, size_t blocks)
{
    uint8_t i;
    bool ok;

    if (blocks <= mr->blocks) {
        return true;
    }

    ok = true;

    for (i = 0; i < HDRG_MR_INFLIGHT; i++) {
        hdrg_gk_buffer_free(mr->gk, mr->contrast[i]);
        mr->contrast[i] = hdrg_gk_buffer(mr->gk, blocks * sizeof(int32_t));
        ok = ok && mr->contrast[i] != NULL;
    }

    mr->blocks = ok ? blocks : 0u;

    return ok;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mr_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const contrasts[2] = {"source", "contrast"}, *const dering[3] = {"source",
                                                                                 "contrast", "dst"};
    hdrg_mr_t *mr;
    hdrg_status_t status;

    mr = (hdrg_mr_t *)api->mem_calloc(api, 1, sizeof(*mr));
    if (mr == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &mr->strength);
    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_MR_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &mr->gk);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, mr->gk->ops->kind);
    }

    if (status == HDRG_OK) {
        mr->contrasts = hdrg_gk_kernel(mr->gk, &hdrg_gk_dering_contrasts, "params", contrasts, 2);
        mr->dering = hdrg_gk_kernel(mr->gk, &hdrg_gk_dering_dering, "params", dering, 3);
        status = (mr->contrasts != NULL && mr->dering != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_mr_release(mr, api);

        return status;
    }

    *inst = mr;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_mr_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_mr_release((hdrg_mr_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mr_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_mr_t *mr = (hdrg_mr_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &mr->width, &mr->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || mr->width == 0 || mr->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_dering_settings(nego, api, mr->strength, &mr->reach, &mr->floor, &mr->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, mr->width, mr->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !mr->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mr_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_mr_t *mr = (hdrg_mr_t *)inst;
    hdrg_gk_buffer_t *buffers[3];
    hdrg_mr_params_t params;
    hdrg_gk_pass_t *pass;
    hdrg_gk_job_t frames;
    hdrg_status_t status;
    uint32_t rows;

    status = hdrg_gk_job_frames(mr->gk, &mr->device, job, api, mr->width, mr->height, 6u, &frames);
    if (status != HDRG_OK) {
        return status;
    }

    memset(&params, 0, sizeof(params));
    params.width = mr->width;
    params.height = mr->height;
    params.in_stride = (uint32_t)(frames.in_stride / 4u);
    params.out_stride = (uint32_t)(frames.out_stride / 4u);
    params.blocks = (mr->width + HDRG_MR_GRID - 1u) / HDRG_MR_GRID;
    params.reach = mr->reach;
    params.floor = mr->floor;
    rows = (mr->height + HDRG_MR_GRID - 1u) / HDRG_MR_GRID;

    if (!hdrg_mr_buffers(mr, (size_t)params.blocks * rows)) {
        return HDRG_ERR_NOMEM;
    }

    pass = hdrg_gk_begin(mr->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    buffers[0] = frames.in;
    buffers[1] = mr->contrast[mr->slot];
    buffers[2] = frames.out;
    mr->slot = (mr->slot + 1u) % HDRG_MR_INFLIGHT;

    hdrg_gk_dispatch(mr->gk, pass, mr->contrasts, &params, sizeof(params), buffers,
                     (params.blocks + HDRG_MR_GROUP - 1u) / HDRG_MR_GROUP,
                     (rows + HDRG_MR_GROUP - 1u) / HDRG_MR_GROUP, HDRG_MR_GROUP, HDRG_MR_GROUP);
    hdrg_gk_dispatch(mr->gk, pass, mr->dering, &params, sizeof(params), buffers,
                     ((mr->width + 1u) / 2u + HDRG_MR_GROUP - 1u) / HDRG_MR_GROUP,
                     (mr->height + HDRG_MR_GROUP - 1u) / HDRG_MR_GROUP, HDRG_MR_GROUP,
                     HDRG_MR_GROUP);

    return hdrg_gk_job_commit(mr->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern hdrg_status_t hdrg_gk_dering_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                             const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "dering");
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
        status = api->class_set_create(cls, hdrg_mr_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_mr_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_mr_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_mr_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_frames(cls, backend->frame_alloc, backend->frame_free);
    }

    if (status == HDRG_OK) {
        status = api->class_set_device_release(cls, hdrg_gk_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
