/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "gpu_apply.h"

#include <string.h>

#define HDRG_GA_LUT_NODES   (HDRG_LUT_SIZE * HDRG_LUT_SIZE * HDRG_LUT_SIZE * 3u)
#define HDRG_GA_FLAG_CORED  1u
#define HDRG_GA_FLAG_FIXED  2u
#define HDRG_GA_FLAG_BOOST  4u
#define HDRG_GA_FLAG_BASE   8u
#define HDRG_GA_FLAG_DITHER 16u
#define HDRG_GA_FLAG_LIGHT  32u
#define HDRG_GA_FLAG_GRAIN  64u
#define HDRG_GA_LIGHT_FRAME 10u /* the light's frame words: the ratio's matrix, the SDR's white */
#define HDRG_GA_LIGHT_WORDS (3u + 4u * HDRG_LIGHT_BINS) /* its words after the rows' */

static inline uint32_t hdrg_ga_div(uint32_t n, uint32_t d)
{
    return (n + d - 1u) / d;
}

static inline void hdrg_ga_put_f(uint32_t *words, size_t i, float v)
{
    memcpy(&words[i], &v, sizeof(v));
}

/* the uniform of the apply and its frame (yuv NULL: R'G'B' output) */
static void hdrg_ga_params(const hdrg_apply_t *apply, const hdrg_yuv_t *yuv,
                           hdrg_gpu_apply_params_t *p)
{
    const hdrg_source_t *src = apply->src;
    const hdrg_plan_t *plan = apply->plan;
    const uint32_t cells = plan->fixed ? 1u : plan->grid_w * plan->grid_h;
    uint32_t per;
    float coeffs[10];

    memset(p, 0, sizeof(*p));
    p->width = src->width;
    p->height = src->height;
    p->src_stride = ((uint32_t)src->width * src->comps * (src->wide ? 2u : 1u) + 3u) & ~3u;
    p->comps = src->comps;
    p->wide = src->wide ? 1u : 0u;
    p->entries = src->entries;
    p->pack = (apply->sample_bytes == 2u) ? 2u : 4u;
    p->out_stride = 3u * hdrg_ga_div(src->width, p->pack);
    p->grid_w = plan->fixed ? 1u : plan->grid_w;
    p->grid_h = plan->fixed ? 1u : plan->grid_h;
    p->groups_x = hdrg_ga_div(hdrg_ga_div(src->width, p->pack), HDRG_GC_GROUP);
    p->lut_at = 3u * src->entries;
    p->pq_at = p->lut_at + HDRG_GA_LUT_NODES;
    p->rows_at = 2u * src->width;
    p->boost_at = p->rows_at + 3u * src->height;
    p->near_at = p->boost_at + cells * HDRG_PLAN_BINS;
    p->base_at = p->near_at + cells;
    p->coring_at = p->base_at + cells;

    p->flags = plan->cored ? HDRG_GA_FLAG_CORED : 0u;
    p->flags |= plan->fixed ? HDRG_GA_FLAG_FIXED : 0u;
    p->flags |= (!plan->fixed && plan->boost != NULL) ? HDRG_GA_FLAG_BOOST : 0u;
    p->flags |= (!plan->fixed && plan->base != NULL) ? HDRG_GA_FLAG_BASE : 0u;
    p->flags |= (apply->dither && apply->sample_bytes == 1u) ? HDRG_GA_FLAG_DITHER : 0u;
    p->flags |= (apply->chunk_cll != NULL) ? HDRG_GA_FLAG_LIGHT : 0u;
    p->flags |= (apply->grain != NULL) ? HDRG_GA_FLAG_GRAIN : 0u;
    p->grain_at = p->coring_at + HDRG_PLAN_CORING;
    p->light_at = p->grain_at + (((p->flags & HDRG_GA_FLAG_GRAIN) != 0u) ? HDRG_GRAIN_WORDS : 0u);

    p->max_code = apply->lut->max_code;
    p->diffuse = apply->diffuse;
    p->span = apply->span;
    p->pivot = apply->grade.pivot;
    p->floor = apply->grade.floor;
    p->contrast = apply->grade.contrast;
    p->clarity = apply->grade.clarity;
    p->src_kr = src->coeffs[0];
    p->src_kg = src->coeffs[1];
    p->src_kb = src->coeffs[2];
    p->out_kr = apply->shoulder.coeffs[0];
    p->out_kg = apply->shoulder.coeffs[1];
    p->out_kb = apply->shoulder.coeffs[2];
    p->desat = apply->shoulder.desat;
    p->knee = apply->shoulder.knee;
    p->range = apply->shoulder.range;
    p->white = apply->shoulder.white;
    hdrg_plan_fixed_ramp(&p->fixed_lo, &p->fixed_hi);

    if (yuv == NULL) {
        return;
    }

    p->yuv_bytes = (yuv->bits > 8u) ? 2u : 1u;
    per = (p->yuv_bytes == 2u) ? 2u : 4u;
    p->cw = hdrg_ga_div(src->width, 2u);
    p->ch = hdrg_ga_div(src->height, 2u);
    p->pairs = yuv->interleaved ? 1u : 0u;
    p->shift = yuv->msb ? 16u - yuv->bits : 0u;
    p->y_stride = 2u * hdrg_ga_div(p->cw, per);
    p->c_stride = yuv->interleaved ? p->y_stride : hdrg_ga_div(p->cw, per);
    p->cb_at = p->y_stride * src->height;
    p->cr_at = p->cb_at + (yuv->interleaved ? 0u : p->c_stride * p->ch);

    hdrg_yuv_output_coeffs(yuv, coeffs);
    p->kr = coeffs[0];
    p->kg = coeffs[1];
    p->kb = coeffs[2];
    p->inv_cb = coeffs[3];
    p->inv_cr = coeffs[4];
    p->y_off = coeffs[5];
    p->y_range = coeffs[6];
    p->c_off = coeffs[7];
    p->c_range = coeffs[8];
    p->top = coeffs[9];
}

/* the frame buffer: the grid positions of the columns and rows, the plan's grids, the coring */
static void hdrg_ga_frame(const hdrg_apply_t *apply, const hdrg_gpu_apply_params_t *p,
                          uint32_t *words)
{
    const hdrg_plan_t *plan = apply->plan;
    const uint32_t cells = p->grid_w * p->grid_h;
    hdrg_grid_pos_t pos;
    uint32_t x, y, i;

    for (x = 0; x < p->width; x++) {
        words[x] = plan->fixed ? 0u : apply->grid_x[x];
        hdrg_ga_put_f(words, (size_t)p->width + x, plan->fixed ? 0.0f : apply->grid_tx[x]);
    }

    for (y = 0; y < p->height; y++) {
        memset(&pos, 0, sizeof(pos));

        if (!plan->fixed) {
            hdrg_grid_pos(apply->grid_scale_x, apply->grid_scale_y, plan->grid_w, plan->grid_h, 0,
                          y, &pos);
        }

        words[p->rows_at + y] = pos.y0;
        words[p->rows_at + p->height + y] = pos.y1;
        hdrg_ga_put_f(words, (size_t)p->rows_at + 2u * p->height + y, pos.ty);
    }

    if ((p->flags & HDRG_GA_FLAG_BOOST) != 0u) {
        memcpy(words + p->boost_at, plan->boost, (size_t)cells * HDRG_PLAN_BINS * sizeof(float));
        memcpy(words + p->near_at, plan->boost_near, (size_t)cells * sizeof(float));
    }

    if ((p->flags & HDRG_GA_FLAG_BASE) != 0u) {
        memcpy(words + p->base_at, plan->base, (size_t)cells * sizeof(float));
    }

    for (i = 0; i < HDRG_PLAN_CORING; i++) {
        hdrg_ga_put_f(words, (size_t)p->coring_at + i, plan->fixed ? 0.0f : plan->coring[i]);
    }

    if ((p->flags & HDRG_GA_FLAG_GRAIN) != 0u) {
        hdrg_grain_words(apply->grain, words + p->grain_at);
    }

    for (i = 0; (p->flags & HDRG_GA_FLAG_LIGHT) != 0u && i < 9u; i++) {
        hdrg_ga_put_f(words, (size_t)p->light_at + i, apply->light_matrix[i]);
    }

    if ((p->flags & HDRG_GA_FLAG_LIGHT) != 0u) {
        hdrg_ga_put_f(words, (size_t)p->light_at + 9u, apply->light_white);
    }
}

/* the rows' light into the apply's chunks, in the rows' and workgroups' order */
static void hdrg_ga_light(const hdrg_apply_t *apply, const hdrg_gpu_apply_params_t *p,
                          const uint32_t *words)
{
    uint32_t y, g, chunk;
    double sum;
    float v;

    for (y = 0; y < p->height; y++) {
        chunk = y / HDRG_EXEC_GRAIN_ROWS;
        memcpy(&v, &words[y], sizeof(v));
        apply->chunk_cll[chunk] = HDRG_MAX(apply->chunk_cll[chunk], v);
        sum = 0.0;

        for (g = 0; g < p->groups_x; g++) {
            memcpy(&v, &words[p->height + (size_t)y * p->groups_x + g], sizeof(v));
            sum += (double)v;
        }

        apply->chunk_fall[chunk] += sum;
    }

    /* the frame's light: into the first chunk's */
    words += (size_t)p->height * (1u + p->groups_x);

    for (g = 0; apply->chunk_light != NULL && g < 3u; g++) {
        memcpy(&apply->chunk_light[0].channel[g], &words[g], sizeof(float));
    }

    for (y = 0; apply->chunk_light != NULL && y < HDRG_LIGHT_BINS; y++) {
        apply->chunk_light[0].count[y] = words[3u + y];
        apply->chunk_light[0].samples[y] = words[3u + HDRG_LIGHT_BINS + y];
        apply->chunk_light[0].ratio[y] = words[3u + 2u * HDRG_LIGHT_BINS + y];
        apply->chunk_light[0].place[y] = words[3u + 3u * HDRG_LIGHT_BINS + y];
    }
}

extern void hdrg_gpu_apply_export(const hdrg_apply_t *apply, const hdrg_yuv_t *yuv,
                                  hdrg_gpu_apply_params_t *params, size_t *luts_size,
                                  size_t *frame_size, uint8_t *luts, uint32_t *frame)
{
    const hdrg_source_t *src = apply->src;
    const bool planar = yuv != NULL && yuv->layout == HDRG_YUV_420;

    hdrg_ga_params(apply, planar ? yuv : NULL, params);
    params->flags &= ~HDRG_GA_FLAG_LIGHT;
    *luts_size = ((size_t)params->pq_at + HDRG_PQ_TABLE_ENTRIES) * sizeof(float);
    *frame_size = ((size_t)params->grain_at +
                   (((params->flags & HDRG_GA_FLAG_GRAIN) != 0u) ? HDRG_GRAIN_WORDS : 0u)) *
                  sizeof(uint32_t);

    if (luts != NULL) {
        memcpy(luts, src->lut, (size_t)params->lut_at * sizeof(float));
        memcpy(luts + (size_t)params->lut_at * sizeof(float), apply->lut->nodes,
               (size_t)HDRG_GA_LUT_NODES * sizeof(float));
        memcpy(luts + (size_t)params->pq_at * sizeof(float), hdrg_pq_table, sizeof(hdrg_pq_table));
    }

    if (frame != NULL) {
        hdrg_ga_frame(apply, params, frame);
    }
}

extern hdrg_status_t hdrg_gpu_apply_frame(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec,
                                          const hdrg_apply_t *apply, uint8_t *dst, size_t stride,
                                          const hdrg_yuv_t *yuv, uint8_t source,
                                          const hdrg_device_frame_t *device)
{
    const hdrg_source_t *src = apply->src;
    const bool planar = yuv != NULL && yuv->layout == HDRG_YUV_420, kept = source < HDRG_GC_SLOTS;
    hdrg_gc_dispatch_t dispatches[2];
    hdrg_gpu_apply_params_t params;
    hdrg_status_t status;
    uint32_t in, out;
    uint8_t slots[HDRG_GA_BUFFERS] = {HDRG_GA_SOURCE, HDRG_GA_LUTS,  HDRG_GA_FRAME,
                                      HDRG_GA_DST,    HDRG_GA_LIGHT, HDRG_GA_PLANES},
            *views[HDRG_GA_BUFFERS], i, p;
    size_t sizes[HDRG_GA_BUFFERS], bytes;

    hdrg_ga_params(apply, planar ? yuv : NULL, &params);

    /* a frame kept on the GPU: 16-bit R'G'B' rows of the frames' pitch, read in place */
    if (kept) {
        slots[HDRG_GA_SOURCE] = source;
        params.src_stride = (uint32_t)hdrg_gpu_convert_pitch(src->width);
    }

    sizes[HDRG_GA_SOURCE] = (size_t)params.src_stride * params.height;
    sizes[HDRG_GA_LUTS] = ((size_t)params.pq_at + HDRG_PQ_TABLE_ENTRIES) * sizeof(float);
    sizes[HDRG_GA_FRAME] =
        ((size_t)params.light_at +
         (((params.flags & HDRG_GA_FLAG_LIGHT) != 0u) ? HDRG_GA_LIGHT_FRAME : 0u)) *
        sizeof(uint32_t);
    sizes[HDRG_GA_DST] = (size_t)params.out_stride * params.height * sizeof(uint32_t);
    sizes[HDRG_GA_LIGHT] =
        ((size_t)params.height * (1u + params.groups_x) + HDRG_GA_LIGHT_WORDS) * sizeof(uint32_t);
    sizes[HDRG_GA_PLANES] =
        planar ? ((size_t)params.cr_at + (size_t)params.c_stride * params.ch) * sizeof(uint32_t)
               : sizeof(uint32_t);

    for (i = 0; i < HDRG_GA_BUFFERS; i++) {
        views[i] = NULL;

        /* host views of what the CPU writes or reads back alone (4:2:0 out: not the R'G'B') */
        if (!hdrg_gpu_core_buffer(gpu, slots[i], sizes[i],
                                  ((kept && i == HDRG_GA_SOURCE) ||
                                   (device != NULL && (i == HDRG_GA_DST || i == HDRG_GA_PLANES)) ||
                                   (planar && i == HDRG_GA_DST) || (!planar && i == HDRG_GA_PLANES))
                                      ? NULL
                                      : &views[i])) {
            return HDRG_ERR_NOMEM;
        }
    }

    in = (kept ? 0u : (1u << HDRG_GA_SOURCE)) | (1u << HDRG_GA_FRAME) | (1u << HDRG_GA_LIGHT);

    /* the source's tables and the color LUT stay for the session's frames */
    if (gpu->eotf != src->lut || gpu->entries != src->entries) {
        memcpy(views[HDRG_GA_LUTS], src->lut, (size_t)params.lut_at * sizeof(float));
        memcpy(views[HDRG_GA_LUTS] + (size_t)params.lut_at * sizeof(float), apply->lut->nodes,
               (size_t)HDRG_GA_LUT_NODES * sizeof(float));
        memcpy(views[HDRG_GA_LUTS] + (size_t)params.pq_at * sizeof(float), hdrg_pq_table,
               sizeof(hdrg_pq_table));
        in |= 1u << HDRG_GA_LUTS;
        gpu->eotf = src->lut;
        gpu->entries = src->entries;
    }

    if (!kept) {
        bytes = (size_t)src->width * src->comps * (src->wide ? 2u : 1u);
        hdrg_gpu_core_copy(exec, hdrg_source_row(src, 0), src->stride, views[HDRG_GA_SOURCE],
                           params.src_stride, bytes, src->height);
    }

    hdrg_ga_frame(apply, &params, (uint32_t *)(void *)views[HDRG_GA_FRAME]);
    memset(views[HDRG_GA_LIGHT], 0, sizes[HDRG_GA_LIGHT]);

    dispatches[0].entry = 0;
    dispatches[0].groups_x = params.groups_x;
    dispatches[0].groups_y = params.height;
    dispatches[1].entry = 1;
    dispatches[1].groups_x = hdrg_ga_div(hdrg_gpu_apply_yuv_threads(&params), HDRG_GC_GROUP);
    dispatches[1].groups_y = params.ch;
    out = (device != NULL) ? 0u : planar ? (1u << HDRG_GA_PLANES) : (1u << HDRG_GA_DST);
    out |= ((params.flags & HDRG_GA_FLAG_LIGHT) != 0u) ? (1u << HDRG_GA_LIGHT) : 0u;

    if (!hdrg_gpu_core_run(gpu, HDRG_GC_APPLY, &params, slots, dispatches, planar ? 2u : 1u, in,
                           out)) {
        gpu->eotf = NULL;

        return HDRG_ERR_INTERNAL;
    }

    status = HDRG_OK;

    /* the app's planes on the GPU written there, else the host's read back */
    if (device != NULL && planar) {
        status = hdrg_gpu_core_plane(gpu, device, 0, HDRG_GA_PLANES, 0, params.y_stride,
                                     params.width * params.yuv_bytes, params.height,
                                     hdrg_gc_texel(params.yuv_bytes, false), false);

        for (p = 1; status == HDRG_OK && p < ((params.pairs != 0u) ? 2u : 3u); p++) {
            status = hdrg_gpu_core_plane(
                gpu, device, p, HDRG_GA_PLANES, (p == 1u) ? params.cb_at : params.cr_at,
                params.c_stride, params.cw * params.yuv_bytes * ((params.pairs != 0u) ? 2u : 1u),
                params.ch, hdrg_gc_texel(params.yuv_bytes, params.pairs != 0u), false);
        }
    } else if (device != NULL) {
        status = hdrg_gpu_core_plane(gpu, device, 0, HDRG_GA_DST, 0, params.out_stride,
                                     params.width * 3u * apply->sample_bytes, params.height,
                                     HDRG_GC_TEXEL_R8, false);
    } else if (planar) {
        bytes = params.yuv_bytes;
        hdrg_gpu_core_copy(exec, views[HDRG_GA_PLANES], (size_t)params.y_stride * sizeof(uint32_t),
                           (uint8_t *)(uintptr_t)yuv->planes[0], yuv->strides[0],
                           (size_t)params.width * bytes, params.height);

        for (p = 1; p < ((params.pairs != 0u) ? 2u : 3u); p++) {
            hdrg_gpu_core_copy(
                exec,
                views[HDRG_GA_PLANES] +
                    (size_t)((p == 1u) ? params.cb_at : params.cr_at) * sizeof(uint32_t),
                (size_t)params.c_stride * sizeof(uint32_t), (uint8_t *)(uintptr_t)yuv->planes[p],
                yuv->strides[p], (size_t)params.cw * bytes * ((params.pairs != 0u) ? 2u : 1u),
                params.ch);
        }
    } else {
        hdrg_gpu_core_copy(exec, views[HDRG_GA_DST], (size_t)params.out_stride * sizeof(uint32_t),
                           dst, stride, (size_t)params.width * 3u * apply->sample_bytes,
                           params.height);
    }

    if (status == HDRG_OK && (params.flags & HDRG_GA_FLAG_LIGHT) != 0u) {
        hdrg_ga_light(apply, &params, (const uint32_t *)(const void *)views[HDRG_GA_LIGHT]);
    }

    return status;
}

extern uint32_t hdrg_gpu_apply_yuv_threads(const hdrg_gpu_apply_params_t *params)
{
    return (params->pairs != 0u) ? params->c_stride / 2u : params->c_stride;
}
