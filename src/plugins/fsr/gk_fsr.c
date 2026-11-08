/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * <prefix>.fsr1 ports AMD FidelityFX Super Resolution 1.0.2 (MIT; its
 * notice is in fsr.h).
 */

#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "fsr.h"
#include "parts.h"

#define HDRG_MF_LIT(s)   s, sizeof(s) - 1u
#define HDRG_MF_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */
#define HDRG_MF_GROUP    8u /* the shader's workgroups: 8 x 8 */
#define HDRG_MF_MID      1u /* Params.flags: EASU into mid (RCAS follows) */
#define HDRG_MF_COPY     2u /* ... the source's codes (the same size) */

/*
 * hdrg.scale.fsr1 (src/plugins/fsr/fsr.c) on Metal and Vulkan
 * (hdrgrading/gpu/filter_gk.h): the WGSL of src/plugins/fsr/wgsl/fsr.wgsl through Tint
 * (src/plugins/fsr/msl/fsr_*, src/plugins/fsr/spirv/fsr_*), within the CPU's output
 * (the GPU's divisions may round otherwise). The jobs' RGB16 frames are the
 * plugin's host buffers (the backend's frame_alloc), read and written by the
 * GPU in place; EASU, and with sharpen RCAS over EASU's floats, in a pass
 * ended by its completion. ASYNC, HDRG_MF_INFLIGHT frames in flight, each
 * with its floats of its own.
 */
typedef struct {
    const hdrg_gk_t *gk; /* first: the backend's frame_alloc's */
    hdrg_gk_kernel_t *easu, *rcas;
    hdrg_gk_buffer_t *mid[HDRG_MF_INFLIGHT];
    size_t mid_size;                   /* the floats' buffers' (0: none yet) */
    uint32_t in_w, in_h, out_w, out_h; /* configure's */
    uint32_t slot;                     /* the next job's floats buffer */
    bool sharpen;
    float scale[2], offset[2], sharpness;
} hdrg_mf_t;

/* the shader's uniform (Params, padded to 64 bytes) */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h, in_stride, out_stride, flags, pad;
    float scale_x, scale_y, offset_x, offset_y, sharpness, unused[3];
} hdrg_mf_params_t;

static void hdrg_mf_release(hdrg_mf_t *mf, const hdrg_plugin_api_t *api)
{
    uint8_t i;

    hdrg_gk_kernel_free(mf->gk, mf->easu);
    hdrg_gk_kernel_free(mf->gk, mf->rcas);

    for (i = 0; i < HDRG_MF_INFLIGHT; i++) {
        hdrg_gk_buffer_free(mf->gk, mf->mid[i]);
    }

    hdrg_gk_plugin_release(api, mf->gk);
    api->mem_free(api, mf);
}

/* the floats buffers of every slot for size bytes, kept while they are large enough */
static bool hdrg_mf_buffers(hdrg_mf_t *mf, size_t size)
{
    uint8_t i;
    bool ok;

    if (size <= mf->mid_size) {
        return true;
    }

    ok = true;

    for (i = 0; i < HDRG_MF_INFLIGHT; i++) {
        hdrg_gk_buffer_free(mf->gk, mf->mid[i]);
        mf->mid[i] = hdrg_gk_buffer(mf->gk, size);
        ok = ok && mf->mid[i] != NULL;
    }

    mf->mid_size = ok ? size : 0u;

    return ok;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mf_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const easu[3] = {"source", "mid", "dst"}, *const rcas[2] = {"mid", "dst"};
    hdrg_mf_t *mf;
    hdrg_status_t status;
    int64_t sharpen;
    double stops;

    mf = (hdrg_mf_t *)api->mem_calloc(api, 1, sizeof(*mf));
    if (mf == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_int(options, HDRG_MF_LIT("sharpen"), &sharpen);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_MF_LIT("sharpness"), &stops);
    }

    if (status == HDRG_OK) {
        mf->sharpen = sharpen != 0;
        mf->sharpness = hdrg_math_exp2f(-(float)stops);
        status = api->filter_set_max_inflight(filter, HDRG_MF_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &mf->gk);
    }

    if (status == HDRG_OK) {
        mf->easu = hdrg_gk_kernel(mf->gk, &hdrg_gk_fsr_easu, "params", easu, 3);
        mf->rcas = hdrg_gk_kernel(mf->gk, &hdrg_gk_fsr_rcas, "params", rcas, 2);
        status = (mf->easu != NULL && mf->rcas != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_mf_release(mf, api);

        return status;
    }

    *inst = mf;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_mf_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_mf_release((hdrg_mf_t *)inst, api);
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

    if (format != HDRG_FORMAT_RGB16 || mf->in_w == 0 || mf->in_h == 0 || mf->out_w < mf->in_w ||
        mf->out_h < mf->in_h) {
        return HDRG_ERR_UNSUPPORTED;
    }

    hdrg_fsr_easu_con(mf->in_w, mf->out_w, &mf->scale[0], &mf->offset[0]);
    hdrg_fsr_easu_con(mf->in_h, mf->out_h, &mf->scale[1], &mf->offset[1]);

    return api->nego_set_output(nego, mf->out_w, mf->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mf_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_mf_t *mf = (hdrg_mf_t *)inst;
    hdrg_gk_buffer_t *buffers[3];
    hdrg_mf_params_t params;
    hdrg_gk_pass_t *pass;
    hdrg_gk_job_t frames;
    hdrg_status_t status;
    uint32_t begin, end, row_begin, row_end, groups_x, groups_y;
    bool same, sharpen;
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

    if (status == HDRG_OK && (begin != 0u || end != mf->out_h || row_begin != 0u ||
                              row_end != mf->in_h || in_handle == NULL || out_handle == NULL ||
                              frames.in_stride % 4u != 0u || frames.out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        return status;
    }

    /* the floats only with RCAS (else a placeholder the shader leaves alone) */
    same = mf->out_w == mf->in_w && mf->out_h == mf->in_h;
    sharpen = mf->sharpen && !same;

    if (!hdrg_mf_buffers(mf, sharpen ? (size_t)mf->out_w * mf->out_h * 3u * sizeof(float) : 16u)) {
        return HDRG_ERR_NOMEM;
    }

    pass = hdrg_gk_begin(mf->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memset(&params, 0, sizeof(params));
    params.in_w = mf->in_w;
    params.in_h = mf->in_h;
    params.out_w = mf->out_w;
    params.out_h = mf->out_h;
    params.in_stride = (uint32_t)(frames.in_stride / 4u);
    params.out_stride = (uint32_t)(frames.out_stride / 4u);
    params.flags = same ? HDRG_MF_COPY : sharpen ? HDRG_MF_MID : 0u;
    params.scale_x = mf->scale[0];
    params.scale_y = mf->scale[1];
    params.offset_x = mf->offset[0];
    params.offset_y = mf->offset[1];
    params.sharpness = mf->sharpness;
    frames.in = (hdrg_gk_buffer_t *)in_handle;
    frames.out = (hdrg_gk_buffer_t *)out_handle;
    buffers[0] = frames.in;
    buffers[1] = mf->mid[mf->slot];
    buffers[2] = frames.out;
    mf->slot = (mf->slot + 1u) % HDRG_MF_INFLIGHT;
    groups_x = ((mf->out_w + 1u) / 2u + HDRG_MF_GROUP - 1u) / HDRG_MF_GROUP;
    groups_y = (mf->out_h + HDRG_MF_GROUP - 1u) / HDRG_MF_GROUP;

    hdrg_gk_dispatch(mf->gk, pass, mf->easu, &params, sizeof(params), buffers, groups_x, groups_y,
                     HDRG_MF_GROUP, HDRG_MF_GROUP);

    if (sharpen) {
        hdrg_gk_dispatch(mf->gk, pass, mf->rcas, &params, sizeof(params), buffers + 1, groups_x,
                         groups_y, HDRG_MF_GROUP, HDRG_MF_GROUP);
    }

    return hdrg_gk_job_commit(mf->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern hdrg_status_t hdrg_gk_fsr_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                          const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "fsr1");
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_int(cls, HDRG_MF_LIT("sharpen"), 0, 0, 1,
                                       HDRG_MF_LIT("RCAS: sharpen EASU's output"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_MF_LIT("sharpness"), 0.2, 0.0, 2.0,
            HDRG_MF_LIT("RCAS's sharpness in stops below the strongest (0 = the strongest)"));
    }

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
