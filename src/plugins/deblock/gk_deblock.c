/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)      s, sizeof(s) - 1u
#define HDRG_MB_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */
#define HDRG_MB_GROUP    8u /* the shader's workgroups: 8 x 8 */
#define HDRG_MB_GRID     8u /* the blocks' grid */
#define HDRG_MB_REACH    4u /* the samples a boundary reads on each side */

/*
 * hdrg.deblock (src/plugins/deblock/deblock.c) on Metal and Vulkan
 * (hdrgrading/gpu/filter_gk.h): the WGSL of hdrg.webgpu.deblock
 * (src/plugins/deblock/wgsl/deblock.wgsl) through Tint (src/plugins/deblock/msl/deblock_*,
 * src/plugins/deblock/spirv/deblock_*), the same thresholds, decisions, filters and
 * passthrough; the output equals the CPU's. The jobs' RGB16 frames are the
 * filter's host buffers (class_set_frames), read and written by the GPU in
 * place; two dispatches a frame, ended by the pass's completion.
 * ASYNC, HDRG_MB_INFLIGHT frames in flight.
 */
typedef struct {
    const hdrg_gk_t *gk;     /* first: the backend's frame_alloc's */
    hdrg_gk_device_t device; /* its frames to the next filter, the others it read */
    hdrg_gk_kernel_t *verticals, *horizontals;
    uint32_t width, height;  /* configure's */
    int32_t alpha, beta, tc; /* configure's: hdrg.deblock's thresholds */
    bool active;
    double strength;
} hdrg_mb_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride;
    int32_t alpha, beta, tc, pad;
} hdrg_mb_params_t;

static void hdrg_mb_release(hdrg_mb_t *mb, const hdrg_plugin_api_t *api)
{
    hdrg_gk_kernel_free(mb->gk, mb->verticals);
    hdrg_gk_kernel_free(mb->gk, mb->horizontals);
    hdrg_gk_device_frames_free(mb->gk, &mb->device);
    hdrg_gk_plugin_release(api, mb->gk);
    api->mem_free(api, mb);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mb_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const verticals[2] = {"source", "dst"}, *const horizontals[1] = {"dst"};
    hdrg_mb_t *mb;
    hdrg_status_t status;

    mb = (hdrg_mb_t *)api->mem_calloc(api, 1, sizeof(*mb));
    if (mb == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &mb->strength);
    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_MB_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &mb->gk);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, mb->gk->ops->kind);
    }

    if (status == HDRG_OK) {
        mb->verticals = hdrg_gk_kernel(mb->gk, &hdrg_gk_deblock_verticals, "params", verticals, 2);
        mb->horizontals =
            hdrg_gk_kernel(mb->gk, &hdrg_gk_deblock_horizontals, "params", horizontals, 1);
        status =
            (mb->verticals != NULL && mb->horizontals != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_mb_release(mb, api);

        return status;
    }

    *inst = mb;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_mb_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_mb_release((hdrg_mb_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mb_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_mb_t *mb = (hdrg_mb_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &mb->width, &mb->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || mb->width == 0 || mb->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_deblock_thresholds(nego, api, mb->strength, &mb->alpha, &mb->beta, &mb->tc,
                                     &mb->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, mb->width, mb->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !mb->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mb_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_mb_t *mb = (hdrg_mb_t *)inst;
    hdrg_gk_buffer_t *buffers[2];
    hdrg_mb_params_t params;
    hdrg_gk_pass_t *pass;
    hdrg_gk_job_t frames;
    hdrg_status_t status;

    status = hdrg_gk_job_frames(mb->gk, &mb->device, job, api, mb->width, mb->height, 6u, &frames);
    if (status != HDRG_OK) {
        return status;
    }

    pass = hdrg_gk_begin(mb->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memset(&params, 0, sizeof(params));
    params.width = mb->width;
    params.height = mb->height;
    params.in_stride = (uint32_t)(frames.in_stride / 4u);
    params.out_stride = (uint32_t)(frames.out_stride / 4u);
    params.alpha = mb->alpha;
    params.beta = mb->beta;
    params.tc = mb->tc;
    buffers[0] = frames.in;
    buffers[1] = frames.out;

    hdrg_gk_dispatch(mb->gk, pass, mb->verticals, &params, sizeof(params), buffers,
                     ((mb->width + HDRG_MB_REACH) / HDRG_MB_GRID + HDRG_MB_GROUP) / HDRG_MB_GROUP,
                     (mb->height + HDRG_MB_GROUP - 1u) / HDRG_MB_GROUP, HDRG_MB_GROUP,
                     HDRG_MB_GROUP);
    hdrg_gk_dispatch(mb->gk, pass, mb->horizontals, &params, sizeof(params), buffers + 1,
                     ((mb->width + 1u) / 2u + HDRG_MB_GROUP - 1u) / HDRG_MB_GROUP,
                     (mb->height / HDRG_MB_GRID + HDRG_MB_GROUP) / HDRG_MB_GROUP, HDRG_MB_GROUP,
                     HDRG_MB_GROUP);

    return hdrg_gk_job_commit(mb->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern hdrg_status_t hdrg_gk_deblock_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                              const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "deblock");
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status =
        api->class_add_option_float(cls, HDRG_LIT("strength"), 1.0, 0.0, 4.0,
                                    HDRG_LIT("thresholds over the measured block step (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_mb_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_mb_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_mb_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_mb_submit);
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
