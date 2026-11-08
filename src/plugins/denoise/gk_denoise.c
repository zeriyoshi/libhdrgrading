/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "parts.h"

#define HDRG_MN_LIT(s)   s, sizeof(s) - 1u
#define HDRG_MN_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */
#define HDRG_MN_GROUP    8u /* the shader's workgroups: 8 x 8 */

/*
 * hdrg.denoise.spatial (src/plugins/denoise/denoise.c) on Metal and
 * Vulkan (hdrgrading/gpu/filter_gk.h): the WGSL of hdrg.webgpu.denoise
 * (src/plugins/denoise/wgsl/denoise.wgsl) through Tint (src/plugins/denoise/msl/denoise_*,
 * src/plugins/denoise/spirv/denoise_*), the same noise, windows, coefficients and
 * passthrough, within the L2 tolerance of the CPU's. The jobs' RGB16
 * frames are the plugin's host buffers (the backend's frame_alloc), read and
 * written by the GPU in place; two dispatches a frame, ended by the pass's
 * completion. ASYNC, HDRG_MN_INFLIGHT frames in flight, each with
 * coefficient buffers of its own.
 */
typedef struct {
    const hdrg_gk_t *gk;     /* first: the backend's frame_alloc's */
    hdrg_gk_device_t device; /* its frames to the next filter, the others it read */
    hdrg_gk_kernel_t *coefs, *guide;
    hdrg_gk_buffer_t *coef_a[HDRG_MN_INFLIGHT], *coef_b[HDRG_MN_INFLIGHT];
    size_t pixels;          /* the coefficient buffers' (0: none yet) */
    uint32_t width, height; /* configure's */
    uint32_t slot;          /* the next job's coefficient buffers */
    bool active;
    double strength, chroma, sigma, chroma_sigma[2], level[HDRG_DENOISE_BINS];
    int64_t radius;
} hdrg_mn_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride, radius, pad[3];
    float sigma[4], level[HDRG_DENOISE_BINS];
} hdrg_mn_params_t;

static void hdrg_mn_release(hdrg_mn_t *mn, const hdrg_plugin_api_t *api)
{
    uint8_t i;

    hdrg_gk_kernel_free(mn->gk, mn->coefs);
    hdrg_gk_kernel_free(mn->gk, mn->guide);

    for (i = 0; i < HDRG_MN_INFLIGHT; i++) {
        hdrg_gk_buffer_free(mn->gk, mn->coef_a[i]);
        hdrg_gk_buffer_free(mn->gk, mn->coef_b[i]);
    }

    hdrg_gk_device_frames_free(mn->gk, &mn->device);
    hdrg_gk_plugin_release(api, mn->gk);
    api->mem_free(api, mn);
}

/* the coefficient buffers of every slot for pixels, kept while they are large enough */
static bool hdrg_mn_buffers(hdrg_mn_t *mn, size_t pixels)
{
    uint8_t i;
    bool ok;

    if (pixels <= mn->pixels) {
        return true;
    }

    ok = true;

    for (i = 0; i < HDRG_MN_INFLIGHT; i++) {
        hdrg_gk_buffer_free(mn->gk, mn->coef_a[i]);
        hdrg_gk_buffer_free(mn->gk, mn->coef_b[i]);
        mn->coef_a[i] = hdrg_gk_buffer(mn->gk, pixels * 3u * sizeof(float));
        mn->coef_b[i] = hdrg_gk_buffer(mn->gk, pixels * 3u * sizeof(float));
        ok = ok && mn->coef_a[i] != NULL && mn->coef_b[i] != NULL;
    }

    mn->pixels = ok ? pixels : 0u;

    return ok;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mn_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const coefs[3] = {"source", "coef_a", "coef_b"}, *const guide[4] = {
                                                                     "source", "coef_a", "coef_b",
                                                                     "dst"};
    hdrg_mn_t *mn;
    hdrg_status_t status;

    mn = (hdrg_mn_t *)api->mem_calloc(api, 1, sizeof(*mn));
    if (mn == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_MN_LIT("strength"), &mn->strength);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_MN_LIT("chroma"), &mn->chroma);
    }

    if (status == HDRG_OK) {
        status = api->option_get_int(options, HDRG_MN_LIT("radius"), &mn->radius);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_MN_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &mn->gk);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, mn->gk->ops->kind);
    }

    if (status == HDRG_OK) {
        mn->coefs = hdrg_gk_kernel(mn->gk, &hdrg_gk_denoise_coefs, "params", coefs, 3);
        mn->guide = hdrg_gk_kernel(mn->gk, &hdrg_gk_denoise_guide, "params", guide, 4);
        status = (mn->coefs != NULL && mn->guide != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_mn_release(mn, api);

        return status;
    }

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

    status = api->nego_input(nego, &mn->width, &mn->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || mn->width == 0 || mn->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_denoise_settings(nego, api, mn->strength, &mn->sigma, mn->level, mn->chroma_sigma,
                                   &mn->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, mn->width, mn->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !mn->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mn_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_mn_t *mn = (hdrg_mn_t *)inst;
    hdrg_gk_buffer_t *buffers[4];
    hdrg_mn_params_t params;
    hdrg_gk_pass_t *pass;
    hdrg_gk_job_t frames;
    hdrg_status_t status;
    uint32_t i, groups_y;

    status = hdrg_gk_job_frames(mn->gk, &mn->device, job, api, mn->width, mn->height, 6u, &frames);
    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_mn_buffers(mn, (size_t)mn->width * mn->height)) {
        return HDRG_ERR_NOMEM;
    }

    pass = hdrg_gk_begin(mn->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memset(&params, 0, sizeof(params));
    params.width = mn->width;
    params.height = mn->height;
    params.in_stride = (uint32_t)(frames.in_stride / 4u);
    params.out_stride = (uint32_t)(frames.out_stride / 4u);
    params.radius = (uint32_t)mn->radius;
    params.sigma[0] = (float)mn->sigma;
    params.sigma[1] = (float)(mn->chroma_sigma[0] * mn->strength * mn->chroma);
    params.sigma[2] = (float)(mn->chroma_sigma[1] * mn->strength * mn->chroma);
    params.sigma[3] = (float)mn->strength;

    for (i = 0; i < HDRG_DENOISE_BINS; i++) {
        params.level[i] = (float)mn->level[i];
    }

    buffers[0] = frames.in;
    buffers[1] = mn->coef_a[mn->slot];
    buffers[2] = mn->coef_b[mn->slot];
    buffers[3] = frames.out;
    mn->slot = (mn->slot + 1u) % HDRG_MN_INFLIGHT;
    groups_y = (mn->height + HDRG_MN_GROUP - 1u) / HDRG_MN_GROUP;

    hdrg_gk_dispatch(mn->gk, pass, mn->coefs, &params, sizeof(params), buffers,
                     (mn->width + HDRG_MN_GROUP - 1u) / HDRG_MN_GROUP, groups_y, HDRG_MN_GROUP,
                     HDRG_MN_GROUP);
    hdrg_gk_dispatch(mn->gk, pass, mn->guide, &params, sizeof(params), buffers,
                     ((mn->width + 1u) / 2u + HDRG_MN_GROUP - 1u) / HDRG_MN_GROUP, groups_y,
                     HDRG_MN_GROUP, HDRG_MN_GROUP);

    return hdrg_gk_job_commit(mn->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern hdrg_status_t hdrg_gk_denoise_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                              const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "denoise");
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_MN_LIT("strength"), 1.0, 0.0, 4.0,
        HDRG_MN_LIT("smoothing threshold over the measured noise level (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_MN_LIT("chroma"), 2.0, 0.0, 4.0,
            HDRG_MN_LIT("the chroma threshold over the luma's (chroma noise reads worse)"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(cls, HDRG_MN_LIT("radius"), 2, 1, 4,
                                           HDRG_MN_LIT("window radius in pixels"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
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
        status = api->class_set_device_release(cls, hdrg_gk_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
