/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "metalfx.h"

#define HDRG_LIT(s)      s, sizeof(s) - 1u
#define HDRG_MF_PLUGIN   "hdrg.metalfx"
#define HDRG_MF_ID       "hdrg.metal.metalfx"
#define HDRG_MF_LICENSE  "Apache-2.0 OR MIT"
#define HDRG_MF_COPY     "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_MF_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */

/*
 * MetalFX's spatial scaler (macOS 13, iOS 16) as a SCALE filter: a scaler
 * of Apple's own, not the built-in Lanczos (its values match no CPU filter;
 * it sharpens). The jobs' RGBA16 frames are the filter's host buffers
 * (class_set_frames), copied by the GPU to and from private RGBA16Unorm
 * textures around the scaler (perceptual: the source's code values); a
 * command buffer a frame, ended by its completion. Upscaling only (the
 * scaler is made for it). ASYNC, HDRG_MF_INFLIGHT frames in flight, each
 * with textures of its own.
 */
typedef struct {
    const hdrg_gk_t *gk; /* first: the backend's frame_alloc's */
    hdrg_metal_t *metal;
    hdrg_metal_spatial_t *spatial;
    hdrg_metal_texture_t *in[HDRG_MF_INFLIGHT], *out[HDRG_MF_INFLIGHT];
    uint32_t in_w, in_h, out_w, out_h; /* configure's */
    uint32_t made[4];                  /* the scaler's and textures' sizes (0: none yet) */
    uint32_t slot;                     /* the next job's textures */
} hdrg_mf_t;

/* a submitted job: what its end needs */
typedef struct {
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
} hdrg_mf_work_t;

static void hdrg_mf_done(void *arg, bool ok)
{
    hdrg_mf_work_t *work = (hdrg_mf_work_t *)arg;

    (void)work->api->job_done(work->job, ok ? HDRG_OK : HDRG_ERR_INTERNAL);
    work->api->mem_free(work->api, work);
}

/* the scaler and textures (a command buffer in flight keeps its own references to them) */
static void hdrg_mf_unmake(hdrg_mf_t *mf)
{
    uint8_t i;

    hdrg_metal_spatial_free(mf->spatial);
    mf->spatial = NULL;

    for (i = 0; i < HDRG_MF_INFLIGHT; i++) {
        hdrg_metal_texture_free(mf->in[i]);
        hdrg_metal_texture_free(mf->out[i]);
        mf->in[i] = NULL;
        mf->out[i] = NULL;
    }

    memset(mf->made, 0, sizeof(mf->made));
}

/* the scaler and textures for configure's sizes */
static bool hdrg_mf_make(hdrg_mf_t *mf)
{
    uint8_t i;
    bool ok;

    if (mf->made[0] == mf->in_w && mf->made[1] == mf->in_h && mf->made[2] == mf->out_w &&
        mf->made[3] == mf->out_h) {
        return true;
    }

    hdrg_mf_unmake(mf);

    mf->spatial =
        hdrg_metal_spatial(mf->metal, HDRG_METAL_RGBA16, mf->in_w, mf->in_h, mf->out_w, mf->out_h);
    ok = mf->spatial != NULL;

    for (i = 0; ok && i < HDRG_MF_INFLIGHT; i++) {
        mf->in[i] = hdrg_metal_texture(mf->metal, HDRG_METAL_RGBA16, mf->in_w, mf->in_h);
        mf->out[i] = hdrg_metal_texture(mf->metal, HDRG_METAL_RGBA16, mf->out_w, mf->out_h);
        ok = mf->in[i] != NULL && mf->out[i] != NULL;
    }

    if (!ok) {
        hdrg_mf_unmake(mf);

        return false;
    }

    mf->made[0] = mf->in_w;
    mf->made[1] = mf->in_h;
    mf->made[2] = mf->out_w;
    mf->made[3] = mf->out_h;

    return true;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mf_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_metal_spatial_t *probe;
    hdrg_mf_t *mf;
    hdrg_status_t status;

    (void)options;

    mf = (hdrg_mf_t *)api->mem_calloc(api, 1, sizeof(*mf));
    if (mf == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->filter_set_input(filter, HDRG_FORMAT_RGBA16, 1u, 0u);
    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_MF_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &mf->gk);
    }

    if (status == HDRG_OK) {
        mf->metal = (hdrg_metal_t *)mf->gk->device;
    }

    /* a device without MetalFX (before macOS 13, a virtual machine's GPU) */
    if (status == HDRG_OK) {
        probe = hdrg_metal_spatial(mf->metal, HDRG_METAL_RGBA16, 16u, 16u, 32u, 32u);
        status = (probe != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
        hdrg_metal_spatial_free(probe);
    }

    if (status != HDRG_OK) {
        hdrg_gk_plugin_release(api, mf->gk);
        api->mem_free(api, mf);

        return status;
    }

    *inst = mf;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_mf_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_mf_t *mf = (hdrg_mf_t *)inst;

    hdrg_mf_unmake(mf);
    hdrg_gk_plugin_release(api, mf->gk);
    api->mem_free(api, mf);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mf_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_mf_t *mf = (hdrg_mf_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &mf->in_w, &mf->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &mf->out_w, &mf->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGBA16 || mf->in_w == 0 || mf->in_h == 0 || mf->out_w < mf->in_w ||
        mf->out_h < mf->in_h) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = api->nego_set_output(nego, mf->out_w, mf->out_h, HDRG_FORMAT_RGBA16);

    if (status == HDRG_OK && mf->out_w == mf->in_w && mf->out_h == mf->in_h) {
        return api->nego_set_passthrough(nego);
    }

    if (status == HDRG_OK && !hdrg_mf_make(mf)) {
        status = HDRG_ERR_NOMEM;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mf_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const uint8_t *in;
    hdrg_mf_t *mf = (hdrg_mf_t *)inst;
    hdrg_mf_work_t *work;
    hdrg_metal_pass_t *pass;
    hdrg_status_t status;
    uint32_t begin, end, row_begin, row_end;
    uint8_t *out;
    size_t in_stride, out_stride;
    void *in_handle, *out_handle;

    status = api->job_rows(job, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_input(job, 0, &in, &in_stride, &row_begin, &row_end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &out, &out_stride);
    }

    if (status == HDRG_OK) {
        status = api->job_input_handle(job, 0, &in_handle);
    }

    if (status == HDRG_OK) {
        status = api->job_output_handle(job, &out_handle);
    }

    if (status == HDRG_OK &&
        (begin != 0u || end != mf->out_h || row_begin != 0u || row_end != mf->in_h ||
         in_handle == NULL || out_handle == NULL || mf->spatial == NULL)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        return status;
    }

    work = (hdrg_mf_work_t *)api->mem_alloc(api, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    pass = hdrg_metal_begin(mf->metal);
    if (pass == NULL) {
        api->mem_free(api, work);

        return HDRG_ERR_INTERNAL;
    }

    work->job = job;
    work->api = api;

    hdrg_metal_copy(pass, (hdrg_metal_buffer_t *)in_handle, in_stride, mf->in[mf->slot], true);
    hdrg_metal_spatial_encode(pass, mf->spatial, mf->in[mf->slot], mf->out[mf->slot]);
    hdrg_metal_copy(pass, (hdrg_metal_buffer_t *)out_handle, out_stride, mf->out[mf->slot], false);
    mf->slot = (mf->slot + 1u) % HDRG_MF_INFLIGHT;

    if (!hdrg_metal_commit(pass, hdrg_mf_done, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_INTERNAL;
    }

    return HDRG_OK;
}

static hdrg_status_t hdrg_mf_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                      const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_LIT(HDRG_MF_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    /* the instances find their backend (hdrg_gk_plugin_device) */
    status = api->class_set_user(cls, (void *)(uintptr_t)backend);
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_mf_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_mf_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_mf_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_mf_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_frames(cls, backend->frame_alloc, backend->frame_free);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_metalfx_init(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_MF_PLUGIN), HDRG_LIT("MetalFX scaler"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_MF_LICENSE), HDRG_LIT(HDRG_MF_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    return (status == HDRG_OK) ? hdrg_mf_register(plugin, api, &hdrg_gk_metal_backend) : status;
}
