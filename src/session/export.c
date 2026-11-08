/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "session.h"

/* src/core/wgsl/apply.wgsl (CMakeLists.txt embeds it in every build) */
extern const char hdrg_wgsl_apply[], hdrg_wgsl_convert[];
extern const unsigned char hdrg_spv_apply_apply[], hdrg_spv_apply_yuv[], hdrg_spv_convert_sites[],
    hdrg_spv_convert_slopes[], hdrg_spv_convert_pixels[], hdrg_hlsl_apply_apply[],
    hdrg_hlsl_apply_yuv[], hdrg_hlsl_convert_sites[], hdrg_hlsl_convert_slopes[],
    hdrg_hlsl_convert_pixels[];
extern const size_t hdrg_spv_apply_apply_size, hdrg_spv_apply_yuv_size, hdrg_spv_convert_sites_size,
    hdrg_spv_convert_slopes_size, hdrg_spv_convert_pixels_size;

/* hdrg_shader's entry points: module, name, SPIR-V and its size, HLSL */
typedef struct {
    uint32_t module;
    const char *name;
    const unsigned char *spv;
    const size_t *spv_size;
    const unsigned char *hlsl;
} hdrg_shader_entry_t;

#define HDRG_SHADER_ENTRIES 5u

static const hdrg_shader_entry_t hdrg_shader_entries[HDRG_SHADER_ENTRIES] = {
    {HDRG_SHADER_APPLY, "apply", hdrg_spv_apply_apply, &hdrg_spv_apply_apply_size,
     hdrg_hlsl_apply_apply},
    {HDRG_SHADER_APPLY, "yuv", hdrg_spv_apply_yuv, &hdrg_spv_apply_yuv_size, hdrg_hlsl_apply_yuv},
    {HDRG_SHADER_CONVERT, "sites", hdrg_spv_convert_sites, &hdrg_spv_convert_sites_size,
     hdrg_hlsl_convert_sites},
    {HDRG_SHADER_CONVERT, "slopes", hdrg_spv_convert_slopes, &hdrg_spv_convert_slopes_size,
     hdrg_hlsl_convert_slopes},
    {HDRG_SHADER_CONVERT, "pixels", hdrg_spv_convert_pixels, &hdrg_spv_convert_pixels_size,
     hdrg_hlsl_convert_pixels},
};

extern hdrg_status_t hdrg_session_export(hdrg_session_t *session, const hdrg_yuv_t *yuv,
                                         hdrg_apply_state_t *state)
{
    const hdrg_allocator_t *alloc = &session->ctx->alloc;
    const bool planar = yuv->layout == HDRG_YUV_420;
    hdrg_gpu_apply_params_t *p = &session->export_params;
    hdrg_gc_dispatch_t dispatches[3];
    uint8_t *luts, count, i;
    size_t luts_size, frame_size, sizes[5];

    hdrg_gpu_apply_export(&session->pipeline.apply, yuv, p, &luts_size, &frame_size, NULL, NULL);

    /* Y'CbCr input: the conversion first, its rows the apply's source */
    memset(state->convert_groups, 0, sizeof(state->convert_groups));
    state->convert_params = NULL;
    state->convert_params_size = 0;

    if (session->yuv_in.layout != 0) {
        hdrg_gpu_convert_layout(&session->yuv_in, &session->export_convert, sizes, dispatches,
                                &count);
        state->convert_params = &session->export_convert;
        state->convert_params_size = sizeof(session->export_convert);
        state->yuv_size = sizes[0];
        state->yuv_y_pitch = (size_t)session->export_convert.y_stride * sizeof(uint32_t);
        state->yuv_c_pitch = (size_t)session->export_convert.c_stride * sizeof(uint32_t);
        state->yuv_cb_at = (size_t)session->export_convert.cb_at * sizeof(uint32_t);
        state->yuv_cr_at = (size_t)session->export_convert.cr_at * sizeof(uint32_t);
        state->base_size = sizes[1];
        state->knots_size = sizes[2];
        state->values_size = sizes[3];

        for (i = 0; i < count; i++) {
            state->convert_groups[dispatches[i].entry][0] = dispatches[i].groups_x;
            state->convert_groups[dispatches[i].entry][1] = dispatches[i].groups_y;
        }
    }

    /* the tables stay for the session (its source and color LUT do) */
    luts = NULL;

    if (session->export_luts == NULL) {
        session->export_luts = (uint8_t *)hdrg_mem_alloc(alloc, luts_size);
        session->export_luts_size = luts_size;
        luts = session->export_luts;
    }

    if (frame_size > session->export_frame_cap) {
        hdrg_mem_free(alloc, session->export_frame);
        session->export_frame = (uint32_t *)hdrg_mem_alloc(alloc, frame_size);
        session->export_frame_cap = (session->export_frame != NULL) ? frame_size : 0u;
    }

    /* tables made but not filled are made again the next time (they are filled once) */
    if (session->export_luts == NULL || session->export_frame == NULL) {
        if (luts != NULL) {
            hdrg_mem_free(alloc, luts);
            session->export_luts = NULL;
        }

        return HDRG_ERR_NOMEM;
    }

    hdrg_gpu_apply_export(&session->pipeline.apply, yuv, p, &luts_size, &frame_size, luts,
                          session->export_frame);

    /* Y'CbCr input: the source the conversion's rows */
    if (session->yuv_in.layout != 0) {
        p->src_stride = (uint32_t)hdrg_gpu_convert_pitch(session->yuv_in.width);
    }

    state->layout = HDRG_SHADER_LAYOUT;
    state->params = p;
    state->params_size = sizeof(*p);
    state->luts = session->export_luts;
    state->luts_size = session->export_luts_size;
    state->luts_serial = 1;
    state->frame = session->export_frame;
    state->frame_size = frame_size;
    state->source_pitch = p->src_stride;
    state->dst_pitch = (size_t)p->out_stride * sizeof(uint32_t);
    state->dst_size = state->dst_pitch * p->height;
    state->y_pitch = planar ? (size_t)p->y_stride * sizeof(uint32_t) : 0u;
    state->c_pitch = planar ? (size_t)p->c_stride * sizeof(uint32_t) : 0u;
    state->cb_at = planar ? (size_t)p->cb_at * sizeof(uint32_t) : 0u;
    state->cr_at = planar ? (size_t)p->cr_at * sizeof(uint32_t) : 0u;
    state->planes_size =
        planar ? ((size_t)p->cr_at + ((size_t)p->c_stride * p->ch)) * sizeof(uint32_t) : 4u;
    state->groups[0] = p->groups_x;
    state->groups[1] = p->height;
    state->yuv_groups[0] =
        planar ? (hdrg_gpu_apply_yuv_threads(p) + HDRG_GC_GROUP - 1u) / HDRG_GC_GROUP : 0u;
    state->yuv_groups[1] = planar ? p->ch : 0u;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_receive_state(hdrg_session_t *session, hdrg_session_output_t *out,
                                                hdrg_apply_state_t *state)
{
    hdrg_status_t status;
    uint64_t start;

    if (session == NULL || out == NULL || state == NULL ||
        state->struct_size != sizeof(hdrg_apply_state_t)) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* the app works on its own copy of the frame it sent: nothing may change it */
    if (session->pipeline.prefiltering || session->pipeline.scaling || session->rate_q != 0 ||
        session->config.device != 0 ||
        (session->config.temporal == HDRG_TEMPORAL_TWO_PASS && session->config.passes == NULL)) {
        return HDRG_ERR_UNSUPPORTED;
    }

    start = hdrg_clock_ns();
    status = hdrg_session_receive(session, out, state);
    hdrg_session_clock(session, start);

    if (status == HDRG_OK) {
        session->received++;
        hdrg_session_late(session, out);
    }

    return status;
}

extern const void *hdrg_shader(uint32_t lang, uint32_t module, const char *entry, size_t *size)
{
    const hdrg_shader_entry_t *entries = hdrg_shader_entries;
    const char *text;
    uint8_t i;

    if (lang == HDRG_SHADER_WGSL &&
        (module == HDRG_SHADER_APPLY || module == HDRG_SHADER_CONVERT)) {
        text = (module == HDRG_SHADER_APPLY) ? hdrg_wgsl_apply : hdrg_wgsl_convert;

        if (size != NULL) {
            *size = strlen(text);
        }

        return text;
    }

    for (i = 0; entry != NULL && i < HDRG_SHADER_ENTRIES; i++) {
        if (entries[i].module != module || strcmp(entries[i].name, entry) != 0) {
            continue;
        }

        if (lang == HDRG_SHADER_SPIRV) {
            if (size != NULL) {
                *size = *entries[i].spv_size;
            }

            return entries[i].spv;
        }

        if (lang == HDRG_SHADER_HLSL) {
            if (size != NULL) {
                *size = strlen((const char *)entries[i].hlsl);
            }

            return entries[i].hlsl;
        }
    }

    return NULL;
}
