/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * <prefix>.nis ports NVIDIA Image Scaling 1.0.3 (MIT; its notice is in
 * nis.h).
 */

#include <string.h>

#include "parts.h"
#include "nis.h"

#define HDRG_MN_LIT(s)   s, sizeof(s) - 1u
#define HDRG_MN_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */
#define HDRG_MN_GROUP    8u /* the shader's workgroups: 8 x 8 */
#define HDRG_MN_COPY     1u /* Params.flags: the source's codes (the same size) */

/*
 * hdrg.scale.nis (src/plugins/nis/nis.c) on Metal and Vulkan
 * (hdrgrading/gpu/filter_gk.h): the WGSL of src/plugins/nis/wgsl/nis.wgsl through Tint
 * (src/plugins/nis/msl/nis_*, src/plugins/nis/spirv/nis_*), within the CPU's output
 * (the GPU's divisions may round otherwise). The jobs' RGB16 frames are the
 * plugin's host buffers (the backend's frame_alloc), read and written by the
 * GPU in place; the luma, then the output, in a pass ended by its
 * completion. ASYNC, HDRG_MN_INFLIGHT frames in flight, each with its luma
 * of its own; the tables (NIS_Config.h's) in a host buffer.
 */
typedef struct {
    const hdrg_gk_t *gk; /* first: the backend's frame_alloc's */
    hdrg_gk_kernel_t *luma, *scale;
    hdrg_gk_buffer_t *coefs, *lumas[HDRG_MN_INFLIGHT];
    hdrg_nis_config_t config;          /* configure's */
    size_t luma_size;                  /* the luma buffers' (0: none yet) */
    uint32_t in_w, in_h, out_w, out_h; /* configure's */
    uint32_t slot;                     /* the next job's luma buffer */
    float sharpness;
} hdrg_mn_t;

/* the shader's uniform (Params, padded to 96 bytes) */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h, in_stride, out_stride, flags, pad;
    float scale_x, scale_y, detect_ratio, detect_thres, min_contrast_ratio, ratio_norm,
        contrast_boost, eps, sharp_start_y, sharp_scale_y, sharp_strength_min, sharp_strength_scale,
        sharp_limit_min, sharp_limit_scale, unused[2];
} hdrg_mn_params_t;

static void hdrg_mn_release(hdrg_mn_t *mn, const hdrg_plugin_api_t *api)
{
    uint8_t i;

    hdrg_gk_kernel_free(mn->gk, mn->luma);
    hdrg_gk_kernel_free(mn->gk, mn->scale);
    hdrg_gk_buffer_free(mn->gk, mn->coefs);

    for (i = 0; i < HDRG_MN_INFLIGHT; i++) {
        hdrg_gk_buffer_free(mn->gk, mn->lumas[i]);
    }

    hdrg_gk_plugin_release(api, mn->gk);
    api->mem_free(api, mn);
}

/* the luma buffers of every slot for size bytes, kept while they are large enough */
static bool hdrg_mn_buffers(hdrg_mn_t *mn, size_t size)
{
    uint8_t i;
    bool ok;

    if (size <= mn->luma_size) {
        return true;
    }

    ok = true;

    for (i = 0; i < HDRG_MN_INFLIGHT; i++) {
        hdrg_gk_buffer_free(mn->gk, mn->lumas[i]);
        mn->lumas[i] = hdrg_gk_buffer(mn->gk, size);
        ok = ok && mn->lumas[i] != NULL;
    }

    mn->luma_size = ok ? size : 0u;

    return ok;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mn_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const luma[2] = {"source", "lumas"}, *const scale[4] = {"source", "coefs", "lumas",
                                                                        "dst"};
    hdrg_mn_t *mn;
    hdrg_status_t status;
    uint8_t *contents;
    double sharpness;

    mn = (hdrg_mn_t *)api->mem_calloc(api, 1, sizeof(*mn));
    if (mn == NULL) {
        return HDRG_ERR_NOMEM;
    }

    contents = NULL;
    status = api->option_get_float(options, HDRG_MN_LIT("sharpness"), &sharpness);

    if (status == HDRG_OK) {
        mn->sharpness = (float)sharpness;
        status = api->filter_set_max_inflight(filter, HDRG_MN_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &mn->gk);
    }

    if (status == HDRG_OK) {
        mn->luma = hdrg_gk_kernel(mn->gk, &hdrg_gk_nis_luma, "params", luma, 2);
        mn->scale = hdrg_gk_kernel(mn->gk, &hdrg_gk_nis_scale, "params", scale, 4);
        status = (mn->luma != NULL && mn->scale != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
    }

    if (status == HDRG_OK) {
        mn->coefs = hdrg_gk_host_buffer(mn->gk, 2u * sizeof(hdrg_nis_coef_scale), &contents);
        status = (mn->coefs != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
    }

    if (status != HDRG_OK) {
        hdrg_mn_release(mn, api);

        return status;
    }

    /* the tables: coef_scale, then coef_usm */
    memcpy(contents, hdrg_nis_coef_scale, sizeof(hdrg_nis_coef_scale));
    memcpy(contents + sizeof(hdrg_nis_coef_scale), hdrg_nis_coef_usm, sizeof(hdrg_nis_coef_usm));
    *inst = mn;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_mn_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_mn_release((hdrg_mn_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mn_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_mn_t *mn = (hdrg_mn_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &mn->in_w, &mn->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &mn->out_w, &mn->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || mn->in_w == 0 || mn->in_h == 0 || mn->out_w == 0 ||
        mn->out_h == 0 ||
        !hdrg_nis_setup(mn->sharpness, mn->in_w, mn->in_h, mn->out_w, mn->out_h, &mn->config)) {
        return HDRG_ERR_UNSUPPORTED;
    }

    return api->nego_set_output(nego, mn->out_w, mn->out_h, HDRG_FORMAT_RGB16);
}

/* the uniform of a job's frames */
static void hdrg_mn_params(const hdrg_mn_t *mn, const hdrg_gk_job_t *frames,
                           hdrg_mn_params_t *params)
{
    const hdrg_nis_config_t *c = &mn->config;

    memset(params, 0, sizeof(*params));
    params->in_w = mn->in_w;
    params->in_h = mn->in_h;
    params->out_w = mn->out_w;
    params->out_h = mn->out_h;
    params->in_stride = (uint32_t)(frames->in_stride / 4u);
    params->out_stride = (uint32_t)(frames->out_stride / 4u);
    params->flags = (mn->out_w == mn->in_w && mn->out_h == mn->in_h) ? HDRG_MN_COPY : 0u;
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
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mn_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_mn_t *mn = (hdrg_mn_t *)inst;
    hdrg_gk_buffer_t *buffers[4], *luma[2];
    hdrg_mn_params_t params;
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

    if (status == HDRG_OK && (begin != 0u || end != mn->out_h || row_begin != 0u ||
                              row_end != mn->in_h || in_handle == NULL || out_handle == NULL ||
                              frames.in_stride % 4u != 0u || frames.out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_mn_buffers(mn, (size_t)mn->in_w * mn->in_h * sizeof(float))) {
        return HDRG_ERR_NOMEM;
    }

    pass = hdrg_gk_begin(mn->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    hdrg_mn_params(mn, &frames, &params);
    frames.in = (hdrg_gk_buffer_t *)in_handle;
    frames.out = (hdrg_gk_buffer_t *)out_handle;
    luma[0] = frames.in;
    luma[1] = mn->lumas[mn->slot];
    buffers[0] = frames.in;
    buffers[1] = mn->coefs;
    buffers[2] = mn->lumas[mn->slot];
    buffers[3] = frames.out;
    mn->slot = (mn->slot + 1u) % HDRG_MN_INFLIGHT;

    hdrg_gk_dispatch(mn->gk, pass, mn->luma, &params, sizeof(params), luma,
                     (mn->in_w + HDRG_MN_GROUP - 1u) / HDRG_MN_GROUP,
                     (mn->in_h + HDRG_MN_GROUP - 1u) / HDRG_MN_GROUP, HDRG_MN_GROUP, HDRG_MN_GROUP);
    hdrg_gk_dispatch(mn->gk, pass, mn->scale, &params, sizeof(params), buffers,
                     ((mn->out_w + 1u) / 2u + HDRG_MN_GROUP - 1u) / HDRG_MN_GROUP,
                     (mn->out_h + HDRG_MN_GROUP - 1u) / HDRG_MN_GROUP, HDRG_MN_GROUP,
                     HDRG_MN_GROUP);

    return hdrg_gk_job_commit(mn->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern hdrg_status_t hdrg_gk_nis_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                          const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "nis");
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(cls, HDRG_MN_LIT("sharpness"), 0.5, 0.0, 1.0,
                                         HDRG_MN_LIT("NIS's sharpness (0 = none, 1 = the most)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_mn_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_mn_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_mn_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_mn_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_frames(cls, backend->frame_alloc, backend->frame_free);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
