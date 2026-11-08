/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <string.h>

#include "parts.h"

#define HDRG_ML_LIT(s)   s, sizeof(s) - 1u
#define HDRG_ML_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */
#define HDRG_ML_GROUP    8u /* the shader's workgroups: 8 x 8 */

/*
 * hdrg.scale.lanczos (src/plugins/polyphase/polyphase.c) on Metal and
 * Vulkan (hdrgrading/gpu/filter_gk.h): the WGSL of hdrg.webgpu.lanczos
 * (src/plugins/polyphase/wgsl/lanczos.wgsl) through Tint (src/plugins/polyphase/msl/lanczos_*,
 * src/plugins/polyphase/spirv/lanczos_*), the CPU's tables and integers, the same
 * output. The jobs' RGB16 frames are the plugin's host buffers
 * (the backend's frame_alloc), read and written by the GPU in place; two
 * dispatches a frame, ended by the pass's completion. ASYNC,
 * HDRG_ML_INFLIGHT frames in flight, each with its rows at the source width
 * of its own.
 */
typedef struct {
    const hdrg_gk_t *gk; /* first: the backend's frame_alloc's */
    hdrg_gk_kernel_t *vertical, *horizontal;
    hdrg_gk_buffer_t *taps, *mid[HDRG_ML_INFLIGHT];
    hdrg_polyphase_axis_t rows, cols; /* configure's */
    size_t mid_size;                  /* the rows' buffers' (0: none yet) */
    uint32_t offsets[6];
    uint32_t in_w, in_h, out_w, out_h; /* configure's */
    uint32_t slot;                     /* the next job's rows buffer */
    int32_t antiring;                  /* Q8 */
    int64_t lobes;
} hdrg_ml_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h, in_stride, out_stride, row_taps, col_taps;
    uint32_t rows_index, rows_weight, rows_near, cols_index, cols_weight, cols_near;
    int32_t antiring;
    uint32_t flags;
} hdrg_ml_params_t;

static void hdrg_ml_release(hdrg_ml_t *ml, const hdrg_plugin_api_t *api)
{
    uint8_t i;

    hdrg_gk_kernel_free(ml->gk, ml->vertical);
    hdrg_gk_kernel_free(ml->gk, ml->horizontal);
    hdrg_gk_buffer_free(ml->gk, ml->taps);

    for (i = 0; i < HDRG_ML_INFLIGHT; i++) {
        hdrg_gk_buffer_free(ml->gk, ml->mid[i]);
    }

    hdrg_polyphase_axis_free(api, &ml->rows);
    hdrg_polyphase_axis_free(api, &ml->cols);
    hdrg_gk_plugin_release(api, ml->gk);
    api->mem_free(api, ml);
}

/* the rows buffers of every slot for size bytes, kept while they are large enough */
static bool hdrg_ml_buffers(hdrg_ml_t *ml, size_t size)
{
    uint8_t i;
    bool ok;

    if (size <= ml->mid_size) {
        return true;
    }

    ok = true;

    for (i = 0; i < HDRG_ML_INFLIGHT; i++) {
        hdrg_gk_buffer_free(ml->gk, ml->mid[i]);
        ml->mid[i] = hdrg_gk_buffer(ml->gk, size);
        ok = ok && ml->mid[i] != NULL;
    }

    ml->mid_size = ok ? size : 0u;

    return ok;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_ml_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const vertical[3] = {"source", "taps", "mid"}, *const horizontal[3] = {
                                                                   "taps", "mid", "dst"};
    hdrg_ml_t *ml;
    hdrg_status_t status;
    double strength;

    ml = (hdrg_ml_t *)api->mem_calloc(api, 1, sizeof(*ml));
    if (ml == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_int(options, HDRG_ML_LIT("taps"), &ml->lobes);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_ML_LIT("antiring"), &strength);
    }

    if (status == HDRG_OK) {
        ml->antiring = (int32_t)floor(strength * (double)(1u << HDRG_POLYPHASE_AR) + 0.5);
        status = api->filter_set_max_inflight(filter, HDRG_ML_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &ml->gk);
    }

    if (status == HDRG_OK) {
        ml->vertical = hdrg_gk_kernel(ml->gk, &hdrg_gk_lanczos_vertical, "params", vertical, 3);
        ml->horizontal =
            hdrg_gk_kernel(ml->gk, &hdrg_gk_lanczos_horizontal, "params", horizontal, 3);
        status = (ml->vertical != NULL && ml->horizontal != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_ml_release(ml, api);

        return status;
    }

    *inst = ml;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_ml_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_ml_release((hdrg_ml_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_ml_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_ml_t *ml = (hdrg_ml_t *)inst;
    hdrg_status_t status;
    uint32_t format, in_w, in_h, out_w, out_h;
    uint8_t *contents;
    int32_t *table;
    size_t count;

    status = api->nego_input(nego, &in_w, &in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &out_w, &out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || in_w == 0 || in_h == 0 || out_w == 0 || out_h == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    /* the tables of the sizes, made once into a host buffer the GPU reads (a stage's sizes stay) */
    if (in_w != ml->in_w || in_h != ml->in_h || out_w != ml->out_w || out_h != ml->out_h) {
        status = hdrg_polyphase_axis(api, (uint8_t)ml->lobes, 0.0, 0.0, in_w, out_w, &ml->cols);
        if (status == HDRG_OK) {
            status = hdrg_polyphase_axis(api, (uint8_t)ml->lobes, 0.0, 0.0, in_h, out_h, &ml->rows);
        }

        if (status != HDRG_OK) {
            return status;
        }

        table = hdrg_polyphase_pack(api, &ml->rows, &ml->cols, out_w, out_h, ml->offsets, &count);
        if (table == NULL) {
            return HDRG_ERR_NOMEM;
        }

        hdrg_gk_buffer_free(ml->gk, ml->taps);
        ml->taps = hdrg_gk_host_buffer(ml->gk, count * sizeof(int32_t), &contents);

        if (ml->taps != NULL) {
            memcpy(contents, table, count * sizeof(int32_t));
        }

        api->mem_free(api, table);

        if (ml->taps == NULL) {
            return HDRG_ERR_NOMEM;
        }

        ml->in_w = in_w;
        ml->in_h = in_h;
        ml->out_w = out_w;
        ml->out_h = out_h;
    }

    return api->nego_set_output(nego, out_w, out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_ml_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_ml_t *ml = (hdrg_ml_t *)inst;
    hdrg_gk_buffer_t *buffers[4];
    hdrg_ml_params_t params;
    hdrg_gk_pass_t *pass;
    hdrg_gk_job_t frames;
    hdrg_status_t status;
    uint32_t begin, end, row_begin, row_end, groups_y;
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

    if (status == HDRG_OK && (begin != 0u || end != ml->out_h || row_begin != 0u ||
                              row_end != ml->in_h || in_handle == NULL || out_handle == NULL ||
                              frames.in_stride % 4u != 0u || frames.out_stride % 4u != 0u)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_ml_buffers(ml, (size_t)ml->out_h * ml->in_w * 3u * sizeof(int32_t))) {
        return HDRG_ERR_NOMEM;
    }

    pass = hdrg_gk_begin(ml->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memset(&params, 0, sizeof(params));
    params.in_w = ml->in_w;
    params.in_h = ml->in_h;
    params.out_w = ml->out_w;
    params.out_h = ml->out_h;
    params.in_stride = (uint32_t)(frames.in_stride / 4u);
    params.out_stride = (uint32_t)(frames.out_stride / 4u);
    params.row_taps = ml->rows.taps;
    params.col_taps = ml->cols.taps;
    params.rows_index = ml->offsets[0];
    params.rows_weight = ml->offsets[1];
    params.rows_near = ml->offsets[2];
    params.cols_index = ml->offsets[3];
    params.cols_weight = ml->offsets[4];
    params.cols_near = ml->offsets[5];
    params.antiring = ml->antiring;
    params.flags = ((ml->rows.upscale && ml->antiring > 0) ? 1u : 0u) |
                   ((ml->cols.upscale && ml->antiring > 0) ? 2u : 0u);
    frames.in = (hdrg_gk_buffer_t *)in_handle;
    frames.out = (hdrg_gk_buffer_t *)out_handle;
    buffers[0] = frames.in;
    buffers[1] = ml->taps;
    buffers[2] = ml->mid[ml->slot];
    buffers[3] = frames.out;
    ml->slot = (ml->slot + 1u) % HDRG_ML_INFLIGHT;
    groups_y = (ml->out_h + HDRG_ML_GROUP - 1u) / HDRG_ML_GROUP;

    hdrg_gk_dispatch(ml->gk, pass, ml->vertical, &params, sizeof(params), buffers,
                     (ml->in_w + HDRG_ML_GROUP - 1u) / HDRG_ML_GROUP, groups_y, HDRG_ML_GROUP,
                     HDRG_ML_GROUP);
    hdrg_gk_dispatch(ml->gk, pass, ml->horizontal, &params, sizeof(params), buffers + 1,
                     ((ml->out_w + 1u) / 2u + HDRG_ML_GROUP - 1u) / HDRG_ML_GROUP, groups_y,
                     HDRG_ML_GROUP, HDRG_ML_GROUP);

    return hdrg_gk_job_commit(ml->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern hdrg_status_t hdrg_gk_lanczos_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                              const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "lanczos");
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_int(cls, HDRG_ML_LIT("taps"), 3, 2, 4,
                                       HDRG_ML_LIT("lobes of the Lanczos kernel"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_ML_LIT("antiring"), 1.0, 0.0, 1.0,
            HDRG_ML_LIT("when upscaling, pull each sample toward the range of the source samples "
                        "around it (0 = off)"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_ml_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_ml_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_ml_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_ml_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_frames(cls, backend->frame_alloc, backend->frame_free);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
