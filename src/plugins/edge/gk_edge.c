/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "edge.h"
#include "parts.h"

#define HDRG_ME_LIT(s)   s, sizeof(s) - 1u
#define HDRG_ME_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */
#define HDRG_ME_GROUP    8u /* the shader's workgroups: 8 x 8 */
#define HDRG_ME_COPY     1u /* Params.flags: the source's codes (the same size) */

/*
 * hdrg.scale.edge (src/plugins/edge/edge.c) on Metal and Vulkan
 * (hdrgrading/gpu/filter_gk.h): the WGSL of src/plugins/edge/wgsl/edge.wgsl through Tint
 * (src/plugins/edge/msl/edge_*, src/plugins/edge/spirv/edge_*), within the CPU's
 * output (the GPU's divisions may round otherwise). The jobs' RGB16 frames
 * are the plugin's host buffers (the backend's frame_alloc), read and
 * written by the GPU in place; the luma, then the output, in a pass ended by
 * its completion. ASYNC, HDRG_ME_INFLIGHT frames in flight, each with its
 * luma of its own
 */
typedef struct {
    const hdrg_gk_t *gk; /* first: the backend's frame_alloc's */
    hdrg_gk_kernel_t *luma, *scale;
    hdrg_gk_buffer_t *lumas[HDRG_ME_INFLIGHT];
    hdrg_edge_config_t config;         /* configure's */
    size_t luma_size;                  /* the luma buffers' (0: none yet) */
    uint32_t in_w, in_h, out_w, out_h; /* configure's */
    uint32_t slot;                     /* the next job's luma buffer */
} hdrg_me_t;

/* the shader's uniform (Params, 48 bytes) */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h, in_stride, out_stride, flags, pad;
    float scale_x, scale_y, stretch, sharpen;
} hdrg_me_params_t;

static void hdrg_me_release(hdrg_me_t *me, const hdrg_plugin_api_t *api)
{
    uint8_t i;

    hdrg_gk_kernel_free(me->gk, me->luma);
    hdrg_gk_kernel_free(me->gk, me->scale);

    for (i = 0; i < HDRG_ME_INFLIGHT; i++) {
        hdrg_gk_buffer_free(me->gk, me->lumas[i]);
    }

    hdrg_gk_plugin_release(api, me->gk);
    api->mem_free(api, me);
}

/* the luma buffers of every slot for size bytes, kept while they are large enough */
static bool hdrg_me_buffers(hdrg_me_t *me, size_t size)
{
    uint8_t i;
    bool ok;

    if (size <= me->luma_size) {
        return true;
    }

    ok = true;

    for (i = 0; i < HDRG_ME_INFLIGHT; i++) {
        hdrg_gk_buffer_free(me->gk, me->lumas[i]);
        me->lumas[i] = hdrg_gk_buffer(me->gk, size);
        ok = ok && me->lumas[i] != NULL;
    }

    me->luma_size = ok ? size : 0u;

    return ok;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_me_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const luma[2] = {"source", "lumas"}, *const scale[3] = {"source", "lumas", "dst"};
    hdrg_me_t *me;
    hdrg_status_t status;
    double stretch, sharpen;

    me = (hdrg_me_t *)api->mem_calloc(api, 1, sizeof(*me));
    if (me == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_ME_LIT("stretch"), &stretch);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_ME_LIT("sharpen"), &sharpen);
    }

    if (status == HDRG_OK) {
        me->config.stretch = (float)stretch;
        me->config.sharpen = (float)sharpen;
        status = api->filter_set_max_inflight(filter, HDRG_ME_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &me->gk);
    }

    if (status == HDRG_OK) {
        me->luma = hdrg_gk_kernel(me->gk, &hdrg_gk_edge_luma, "params", luma, 2);
        me->scale = hdrg_gk_kernel(me->gk, &hdrg_gk_edge_scale, "params", scale, 3);
        status = (me->luma != NULL && me->scale != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_me_release(me, api);

        return status;
    }

    *inst = me;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_me_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_me_release((hdrg_me_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_me_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_me_t *me = (hdrg_me_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &me->in_w, &me->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &me->out_w, &me->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || me->in_w == 0 || me->in_h == 0 || me->out_w < me->in_w ||
        me->out_h < me->in_h) {
        return HDRG_ERR_UNSUPPORTED;
    }

    me->config.scale_x = (float)me->in_w / (float)me->out_w;
    me->config.scale_y = (float)me->in_h / (float)me->out_h;

    return api->nego_set_output(nego, me->out_w, me->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_me_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_me_t *me = (hdrg_me_t *)inst;
    hdrg_gk_buffer_t *buffers[3], *luma[2];
    hdrg_me_params_t params;
    hdrg_gk_pass_t *pass;
    hdrg_gk_job_t frames;
    hdrg_status_t status;
    uint32_t begin, end, row_begin, row_end;
    void *in_handle, *out_handle;

    memset(&frames, 0, sizeof(frames));
    status = api->job_rows(job, &begin, &end);

    if (status == HDRG_OK) {
        status = api->job_input(job, 0, &frames.in_host, &frames.in_stride, &row_begin, &row_end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &frames.out_host, &frames.out_stride);
    }

    if (status == HDRG_OK) {
        status = api->job_input_handle(job, 0, &in_handle);
    }

    if (status == HDRG_OK) {
        status = api->job_output_handle(job, &out_handle);
    }

    if (status == HDRG_OK && (begin != 0u || end != me->out_h || row_begin != 0u ||
                              row_end != me->in_h || in_handle == NULL || out_handle == NULL ||
                              frames.in_stride % 4u != 0u || frames.out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_me_buffers(me, (size_t)me->in_w * me->in_h * sizeof(float))) {
        return HDRG_ERR_NOMEM;
    }

    pass = hdrg_gk_begin(me->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memset(&params, 0, sizeof(params));
    params.in_w = me->in_w;
    params.in_h = me->in_h;
    params.out_w = me->out_w;
    params.out_h = me->out_h;
    params.in_stride = (uint32_t)(frames.in_stride / 4u);
    params.out_stride = (uint32_t)(frames.out_stride / 4u);
    params.flags = (me->out_w == me->in_w && me->out_h == me->in_h) ? HDRG_ME_COPY : 0u;
    params.scale_x = me->config.scale_x;
    params.scale_y = me->config.scale_y;
    params.stretch = me->config.stretch;
    params.sharpen = me->config.sharpen;
    frames.in = (hdrg_gk_buffer_t *)in_handle;
    frames.out = (hdrg_gk_buffer_t *)out_handle;
    luma[0] = frames.in;
    luma[1] = me->lumas[me->slot];
    buffers[0] = frames.in;
    buffers[1] = me->lumas[me->slot];
    buffers[2] = frames.out;
    me->slot = (me->slot + 1u) % HDRG_ME_INFLIGHT;

    hdrg_gk_dispatch(me->gk, pass, me->luma, &params, sizeof(params), luma,
                     (me->in_w + HDRG_ME_GROUP - 1u) / HDRG_ME_GROUP,
                     (me->in_h + HDRG_ME_GROUP - 1u) / HDRG_ME_GROUP, HDRG_ME_GROUP, HDRG_ME_GROUP);
    hdrg_gk_dispatch(me->gk, pass, me->scale, &params, sizeof(params), buffers,
                     ((me->out_w + 1u) / 2u + HDRG_ME_GROUP - 1u) / HDRG_ME_GROUP,
                     (me->out_h + HDRG_ME_GROUP - 1u) / HDRG_ME_GROUP, HDRG_ME_GROUP,
                     HDRG_ME_GROUP);

    return hdrg_gk_job_commit(me->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern hdrg_status_t hdrg_gk_edge_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                           const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "edge");
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_ME_LIT("stretch"), HDRG_EDGE_STRETCH, 0.0, 3.0,
        HDRG_ME_LIT("the kernel's widening along an edge, by its coherence (0 = none)"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_ME_LIT("sharpen"), HDRG_EDGE_SHARPEN, 0.0, 2.0,
            HDRG_ME_LIT("the kernel's narrowing across an edge, by its coherence (0 = none)"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_me_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_me_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_me_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_me_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_frames(cls, backend->frame_alloc, backend->frame_free);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
