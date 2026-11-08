/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "gpu_analyze.h"

#include <string.h>

#include "plan.h"

/* the shader's buffers (src/core/wgsl/analyze.wgsl), the core's slots */
#define HDRG_GZ_SOURCE  15u /* the picture's rows uploaded */
#define HDRG_GZ_LUTS    (HDRG_GC_ANALYZE0 + 0u)
#define HDRG_GZ_CELLS   (HDRG_GC_ANALYZE0 + 1u)
#define HDRG_GZ_COUNTS  (HDRG_GC_ANALYZE0 + 2u)
#define HDRG_GZ_LIGHTS  (HDRG_GC_ANALYZE0 + 3u)
#define HDRG_GZ_MOMENTS (HDRG_GC_ANALYZE0 + 4u)
#define HDRG_GZ_BUFFERS 6u
#define HDRG_GZ_ENTRIES 65536u /* a 16-bit source's EOTF table */
#define HDRG_GZ_ROWS    16u    /* hist: rows a workgroup */
#define HDRG_GZ_STRIDE  53u    /* a plan cell's values: the moments, then the grid */

/* the shader's entry points */
#define HDRG_GZ_EDGES      0u
#define HDRG_GZ_CELLS_OF   1u
#define HDRG_GZ_HIST       2u
#define HDRG_GZ_MOMENTS_OF 3u

static inline uint32_t hdrg_gz_div(uint32_t n, uint32_t d)
{
    return (n + d - 1u) / d;
}

static inline uint64_t hdrg_gz_bit(uint8_t slot)
{
    return UINT64_C(1) << slot;
}

/*
 * The picture's params and source slot (its frame's, or its rows uploaded),
 * the EOTF tables (uploaded when new: the work keeps them once a run takes
 * them, hdrg_gz_ran), the buffers the program binds and not this run's (a
 * word): *in the uploads
 */
static hdrg_status_t hdrg_gz_prepare(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec,
                                     hdrg_gpu_analyze_t *work, const hdrg_gpu_picture_t *picture,
                                     hdrg_gpu_analyze_params_t *params, uint8_t *slots,
                                     uint64_t *in)
{
    const hdrg_source_t *src = picture->src;
    const uint8_t spare[4] = {HDRG_GZ_CELLS, HDRG_GZ_COUNTS, HDRG_GZ_LIGHTS, HDRG_GZ_MOMENTS};
    uint8_t *view, i;
    size_t size;

    memset(params, 0, sizeof(*params));
    params->width = picture->width;
    params->height = picture->height;
    params->step_shift = picture->step_shift;
    params->offset = (1u << picture->step_shift) / 2u;
    params->src_width = picture->src_width;
    params->src_height = picture->src_height;
    params->src_stride = picture->stride;
    params->entries = HDRG_GZ_ENTRIES;
    params->kr = src->coeffs[0];
    params->kg = src->coeffs[1];
    params->kb = src->coeffs[2];
    params->floor = HDRG_LUMA_FLOOR;
    *in = 0;

    slots[0] = picture->slot;
    slots[1] = HDRG_GZ_LUTS;
    slots[2] = HDRG_GZ_CELLS;
    slots[3] = HDRG_GZ_COUNTS;
    slots[4] = HDRG_GZ_LIGHTS;
    slots[5] = HDRG_GZ_MOMENTS;

    /* the picture's rows into the source slot, words a row */
    if (picture->slot >= HDRG_GC_SLOTS) {
        params->src_stride = hdrg_gz_div(picture->width * 6u, 4u);
        size = (size_t)params->src_stride * sizeof(uint32_t) * picture->height;

        if (!hdrg_gpu_core_buffer(gpu, HDRG_GZ_SOURCE, size, &view)) {
            return HDRG_ERR_NOMEM;
        }

        hdrg_gpu_core_copy(exec, src->pixels, src->stride, view,
                           (size_t)params->src_stride * sizeof(uint32_t),
                           (size_t)picture->width * 6u, picture->height);
        slots[0] = HDRG_GZ_SOURCE;
        *in |= hdrg_gz_bit(HDRG_GZ_SOURCE);
    }

    size = 3u * HDRG_GZ_ENTRIES * sizeof(float);

    if (!hdrg_gpu_core_buffer(gpu, HDRG_GZ_LUTS, size, &view)) {
        return HDRG_ERR_NOMEM;
    }

    if (work->lut != src->lut) {
        memcpy(view, src->lut, size);
        work->lut = NULL;
        *in |= hdrg_gz_bit(HDRG_GZ_LUTS);
    }

    /*
     * every binding has a buffer (kept while large enough: a word where it has none yet), each
     * with its CPU view as its own runs have it
     */
    for (i = 0; i < 4u; i++) {
        if (!hdrg_gpu_core_buffer(gpu, spare[i], sizeof(uint32_t), &view)) {
            return HDRG_ERR_NOMEM;
        }
    }

    return HDRG_OK;
}

/* a run of the picture done: the EOTF tables it uploaded on the GPU */
static void hdrg_gz_ran(hdrg_gpu_analyze_t *work, const hdrg_gpu_picture_t *picture)
{
    work->lut = picture->src->lut;
}

/* the counts' words: the histogram, the rows' and the columns' maxima */
static size_t hdrg_gz_counts(const hdrg_gpu_picture_t *picture)
{
    return (size_t)HDRG_ANALYZE_BINS + picture->height + picture->width;
}

extern bool hdrg_gpu_analyze_takes(const hdrg_source_t *src)
{
    return src->wide && src->comps == 3u && src->entries == HDRG_GZ_ENTRIES;
}

extern hdrg_status_t hdrg_gpu_analyze_stats(hdrg_gpu_core_t *gpu, const hdrg_allocator_t *alloc,
                                            const hdrg_exec_t *exec, hdrg_gpu_analyze_t *work,
                                            const hdrg_gpu_picture_t *picture, uint8_t cell_shift,
                                            hdrg_analyze_given_t *given)
{
    const size_t words = hdrg_gz_counts(picture);
    hdrg_gpu_analyze_params_t params;
    hdrg_gc_dispatch_t dispatches[2];
    hdrg_status_t status;
    uint64_t in;
    uint32_t *counts, i;
    uint8_t slots[HDRG_GZ_BUFFERS], *view;
    size_t qtotal;

    status = hdrg_gz_prepare(gpu, exec, work, picture, &params, slots, &in);
    if (status != HDRG_OK) {
        return status;
    }

    params.cell_shift = cell_shift;
    params.qw = (picture->width + (1u << cell_shift) - 1u) >> cell_shift;
    params.qh = (picture->height + (1u << cell_shift) - 1u) >> cell_shift;
    qtotal = (size_t)params.qw * params.qh;

    if (!hdrg_gpu_core_buffer(gpu, HDRG_GZ_CELLS, qtotal * HDRG_ANALYZE_CELLS * sizeof(float),
                              &view) ||
        !hdrg_gpu_core_buffer(gpu, HDRG_GZ_COUNTS, words * sizeof(uint32_t),
                              (uint8_t **)(void *)&counts)) {
        return HDRG_ERR_NOMEM;
    }

    given->cells = (const float *)(const void *)view;
    dispatches[0].entry = HDRG_GZ_EDGES;
    dispatches[0].groups_x = hdrg_gz_div(picture->height + picture->width, HDRG_GC_GROUP);
    dispatches[0].groups_y = 1;
    dispatches[1].entry = HDRG_GZ_CELLS_OF;
    dispatches[1].groups_x = hdrg_gz_div(params.qw, HDRG_GC_GROUP);
    dispatches[1].groups_y = params.qh;

    if (!hdrg_gpu_core_run(gpu, HDRG_GC_ANALYZE, &params, slots, dispatches, 2, in,
                           hdrg_gz_bit(HDRG_GZ_CELLS) | hdrg_gz_bit(HDRG_GZ_COUNTS))) {
        return HDRG_ERR_INTERNAL;
    }

    hdrg_gz_ran(work, picture);

    if (picture->height + picture->width > work->edges_cap) {
        hdrg_mem_free(alloc, work->edges);
        work->edges = (float *)hdrg_mem_alloc(alloc, ((size_t)picture->height + picture->width) *
                                                         sizeof(float));
        work->edges_cap = (work->edges != NULL) ? picture->height + picture->width : 0u;
    }

    if (work->edges == NULL) {
        return HDRG_ERR_NOMEM;
    }

    memcpy(work->edges, counts + HDRG_ANALYZE_BINS,
           ((size_t)picture->height + picture->width) * sizeof(float));
    hdrg_analyze_bars(work->edges, work->edges + picture->height, picture->width, picture->height,
                      given->bars);

    /* the histogram of the rectangle within the bars */
    memset(counts, 0, (size_t)HDRG_ANALYZE_BINS * sizeof(uint32_t));
    params.x0 = given->bars[2];
    params.x1 = picture->width - given->bars[3];
    params.y0 = given->bars[0];
    params.y1 = picture->height - given->bars[1];
    dispatches[0].entry = HDRG_GZ_HIST;
    dispatches[0].groups_x = 1;
    dispatches[0].groups_y = hdrg_gz_div(params.y1 - params.y0, HDRG_GZ_ROWS);

    if (!hdrg_gpu_core_run(gpu, HDRG_GC_ANALYZE, &params, slots, dispatches, 1,
                           hdrg_gz_bit(HDRG_GZ_COUNTS), hdrg_gz_bit(HDRG_GZ_COUNTS))) {
        return HDRG_ERR_INTERNAL;
    }

    if (work->hist == NULL) {
        work->hist =
            (uint64_t *)hdrg_mem_alloc(alloc, (size_t)HDRG_ANALYZE_BINS * sizeof(uint64_t));
        if (work->hist == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    for (i = 0; i < HDRG_ANALYZE_BINS; i++) {
        work->hist[i] = counts[i];
    }

    given->hist = work->hist;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_gpu_analyze_moments(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec,
                                              hdrg_gpu_analyze_t *work,
                                              const hdrg_gpu_picture_t *picture,
                                              const hdrg_analysis_t *analysis, uint8_t light_shift,
                                              uint32_t cell, uint32_t grid_w, uint32_t grid_h,
                                              double *sums, float *grid)
{
    const hdrg_plane_t *light = &analysis->light;
    const size_t cells = (size_t)grid_w * grid_h, planes = (size_t)light->width * light->height;
    hdrg_gpu_analyze_params_t params;
    hdrg_gc_dispatch_t dispatch;
    hdrg_status_t status;
    uint64_t in;
    uint8_t slots[HDRG_GZ_BUFFERS], k;
    size_t i;
    float *view, *values;

    status = hdrg_gz_prepare(gpu, exec, work, picture, &params, slots, &in);
    if (status != HDRG_OK) {
        return status;
    }

    params.cell = cell;
    params.grid_w = grid_w;
    params.grid_h = grid_h;
    params.light_shift = light_shift;
    params.light_w = light->width;
    params.light_h = light->height;
    params.has_light = (light->data != NULL) ? 1u : 0u;

    if (light->data != NULL) {
        if (!hdrg_gpu_core_buffer(gpu, HDRG_GZ_LIGHTS, 3u * planes * sizeof(float),
                                  (uint8_t **)(void *)&view)) {
            return HDRG_ERR_NOMEM;
        }

        memcpy(view, light->data, planes * sizeof(float));
        memcpy(view + planes, analysis->light_lo.data, planes * sizeof(float));
        memcpy(view + 2u * planes, analysis->light_hi.data, planes * sizeof(float));
        in |= hdrg_gz_bit(HDRG_GZ_LIGHTS);
    }

    if (!hdrg_gpu_core_buffer(gpu, HDRG_GZ_MOMENTS, cells * HDRG_GZ_STRIDE * sizeof(float),
                              (uint8_t **)(void *)&values)) {
        return HDRG_ERR_NOMEM;
    }

    dispatch.entry = HDRG_GZ_MOMENTS_OF;
    dispatch.groups_x = hdrg_gz_div(grid_w, HDRG_GC_GROUP);
    dispatch.groups_y = grid_h;

    if (!hdrg_gpu_core_run(gpu, HDRG_GC_ANALYZE, &params, slots, &dispatch, 1, in,
                           hdrg_gz_bit(HDRG_GZ_MOMENTS))) {
        return HDRG_ERR_INTERNAL;
    }

    hdrg_gz_ran(work, picture);

    for (i = 0; i < cells; i++) {
        for (k = 0; k < HDRG_PLAN_MOMENTS; k++) {
            sums[i * HDRG_PLAN_MOMENTS + k] = (double)values[i * HDRG_GZ_STRIDE + k];
        }

        memcpy(grid + i * HDRG_PLAN_BINS * 2u, values + i * HDRG_GZ_STRIDE + HDRG_PLAN_MOMENTS,
               HDRG_PLAN_BINS * 2u * sizeof(float));
    }

    return HDRG_OK;
}

extern void hdrg_gpu_analyze_free(const hdrg_allocator_t *alloc, hdrg_gpu_analyze_t *work)
{
    hdrg_mem_free(alloc, work->edges);
    hdrg_mem_free(alloc, work->hist);
    memset(work, 0, sizeof(*work));
}
