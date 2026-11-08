/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_PLAN_H
#define HDRG_CORE_PLAN_H

#include <stdint.h>
#include <string.h>

#include "../kernels/kernels.h"
#include "analyze.h"
#include "exec.h"
#include "image.h"

#define HDRG_SDR_WHITE_NITS 203.0f
#define HDRG_PEAK_CAP_NITS  1000.0f
#define HDRG_GRADE_PIVOT    -2.473931f /* log2(0.18): mid-gray anchor */
#define HDRG_LUMA_FLOOR     6.1035e-5f /* 2^-14 */
#define HDRG_PLAN_CELL      8u         /* grid cell: source pixels per side */
#define HDRG_PLAN_CORING    33u        /* steps of the coring table over La in [0, 1] */
#define HDRG_PLAN_MOMENTS   5u         /* pass B's sums a cell: n, La, Y, Y * h, log2 La */
#define HDRG_PLAN_BINS      24u        /* bins of the boost grid over log2 La ... */
#define HDRG_PLAN_BIN_LOW   -10.0f     /* ... from 2^-10 (darker pixels share the first) to 1 */
#define HDRG_PLAN_SHIFT_MAX 3u         /* a source reduced this much still has a pixel a cell */

/*
 * Grading plan: the display-independent content intent. A
 * pixel's gain in stops is the diffuse level plus its boost share of the
 * headroom left above it, both scaled by the render (hdrg_render_t):
 *   g = diffuse + h(x, y, La) * (headroom - diffuse)
 * The diffuse level is one value for the frame, so diffuse surfaces keep
 * their ratios. The boost share h lives on a bilateral grid (cells of
 * HDRG_PLAN_CELL pixels by HDRG_PLAN_BINS bins of La): sliced at the
 * pixel's position and La, a light's boost does not leak onto a surface of
 * another level next to it (no halo across an outline). base is the clarity
 * base (the blurred log2 La). The apl_* cell statistics let the render fit
 * the APL budget of a display without the source.
 *
 * With noise in the source (the analysis's NOISE.12-5), the
 * apply pass slices the boost, the clarity detail and the highlight
 * desaturation at La cored toward its 3x3 mean: deviations at the noise
 * level are smoothed away, edges and strokes stay; the grading ratio still
 * goes on the pixel.
 *
 * A fixed plan (STATIC) has no grids: one diffuse level and
 * each pixel's boost from its La alone (near white), so the mapping depends
 * on the pixel's code values alone.
 */
typedef struct {
    float diffuse;          /* stops of the diffuse level (before the APL budget) */
    float boost_max;        /* the largest boost share in the grid */
    float *boost;           /* per cell, HDRG_PLAN_BINS bins: the boost share; NULL = none */
    float *boost_near;      /* per cell: the largest share of it and the cells right, below and
                               diagonal (0: a pixel between them takes none) */
    float *base;            /* NULL = clarity off */
    float *apl_y;           /* per cell: mean relative luminance Y */
    float *apl_lift;        /* per cell: contrast lift at the mean La, stops */
    float *apl_boost;       /* per cell: the Y-weighted mean boost share */
    uint32_t width, height; /* source pixels */
    uint32_t grid_w, grid_h, cell;
    float coring[HDRG_PLAN_CORING]; /* (2 sigma)^2 of a pixel's La about its 3x3 mean, by the
                                       mean La: the noise level function in La */
    bool cored;                     /* the apply pass cores La (noise above 8-bit rounding) */
    bool fixed;
} hdrg_plan_t;

static inline void hdrg_plan_pos(const hdrg_plan_t *plan, uint32_t x, uint32_t y,
                                 hdrg_grid_pos_t *pos)
{
    const float inv = 1.0f / (float)plan->cell;

    hdrg_grid_pos(inv, inv, plan->grid_w, plan->grid_h, x, y, pos);
}

/* the coring threshold, squared, of a pixel whose 3x3 mean La is la */
static inline float hdrg_plan_coring(const hdrg_plan_t *plan, float la)
{
    uint32_t i;
    float pos, t;

    pos = HDRG_CLAMP(la, 0.0f, 1.0f) * (float)(HDRG_PLAN_CORING - 1u);
    i = HDRG_MIN((uint32_t)pos, HDRG_PLAN_CORING - 2u);
    t = pos - (float)i;

    return plan->coring[i] * (1.0f - t) + plan->coring[i + 1u] * t;
}

/*
 * the bin position of La on the log2 axis (the exponent plus the linear
 * mantissa: monotonic and continuous, exact IEEE operations), where the
 * boost's ramp lives: a fade or a dark frame keeps its levels apart and its
 * ramps their shape. Its lower bin and the weight of the next.
 */
static inline uint32_t hdrg_plan_bin(float la, float *t)
{
    uint32_t bits, i;
    float v, pos;

    v = HDRG_CLAMP(la, 0.0009765625f, 1.0f);
    memcpy(&bits, &v, sizeof(bits));
    pos = ((float)((int32_t)(bits >> 23) - 127) + (float)(bits & 0x7fffffu) * 0x1p-23f -
           HDRG_PLAN_BIN_LOW) *
          ((float)(HDRG_PLAN_BINS - 1u) / -HDRG_PLAN_BIN_LOW);
    i = HDRG_MIN((uint32_t)pos, HDRG_PLAN_BINS - 2u);
    *t = pos - (float)i;

    return i;
}

/* the boost share at a pixel (hdrg_plan_pos) whose analysis luminance is la */
static inline float hdrg_boost_at(const float *boost, uint32_t grid_w, const hdrg_grid_pos_t *pos,
                                  float la)
{
    const float *c00, *c01, *c10, *c11;
    uint32_t b;
    float t, lo, hi;

    b = hdrg_plan_bin(la, &t);
    c00 = boost + ((size_t)pos->y0 * grid_w + pos->x0) * HDRG_PLAN_BINS + b;
    c01 = boost + ((size_t)pos->y0 * grid_w + pos->x1) * HDRG_PLAN_BINS + b;
    c10 = boost + ((size_t)pos->y1 * grid_w + pos->x0) * HDRG_PLAN_BINS + b;
    c11 = boost + ((size_t)pos->y1 * grid_w + pos->x1) * HDRG_PLAN_BINS + b;
    lo = (c00[0] * (1.0f - pos->tx) + c01[0] * pos->tx) * (1.0f - pos->ty) +
         (c10[0] * (1.0f - pos->tx) + c11[0] * pos->tx) * pos->ty;
    hi = (c00[1] * (1.0f - pos->tx) + c01[1] * pos->tx) * (1.0f - pos->ty) +
         (c10[1] * (1.0f - pos->tx) + c11[1] * pos->tx) * pos->ty;

    return lo + (hi - lo) * t;
}

/* source pixels covered by cell i */
static inline uint32_t hdrg_plan_cell_pixels(const hdrg_plan_t *plan, size_t i)
{
    const uint32_t cx = (uint32_t)(i % plan->grid_w), cy = (uint32_t)(i / plan->grid_w);

    return HDRG_MIN(plan->cell, plan->width - cx * plan->cell) *
           HDRG_MIN(plan->cell, plan->height - cy * plan->cell);
}

/* the diffuse level of a frame of key luminance key under params, stops */
extern float hdrg_plan_diffuse(const hdrg_params_t *params, float key);
/*
 * kernels: the SIMD tier of the boost (NULL = scalar). src is the frame
 * reduced by 2^shift (at most HDRG_PLAN_SHIFT_MAX; a session's proxy):
 * the cells span HDRG_PLAN_CELL >> shift of its pixels, so the grid
 * stays the frame's
 */
extern hdrg_status_t hdrg_plan_build(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                     const hdrg_kernels_t *kernels, const hdrg_source_t *src,
                                     const hdrg_analysis_t *analysis, uint8_t shift,
                                     const hdrg_params_t *params, hdrg_plan_t *out);
/*
 * Pass B's sums from elsewhere (a GPU's, src/core/gpu_analyze): for each of
 * the grid_w x grid_h plan cells of cell pixels of src (row-major), its
 * HDRG_PLAN_MOMENTS sums into sums and its 2 x HDRG_PLAN_BINS boost grid values
 * into grid (zeroed), each pixel's light from the analysis cell of
 * light_shift pixels under it
 */
typedef hdrg_status_t (*hdrg_plan_moments_fn_t)(void *arg, const hdrg_source_t *src,
                                                const hdrg_analysis_t *analysis,
                                                uint8_t light_shift, uint32_t cell, uint32_t grid_w,
                                                uint32_t grid_h, double *sums, float *grid);
/* hdrg_plan_build with pass B's sums from moments (NULL: the CPU's) */
extern hdrg_status_t hdrg_plan_build_with(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                          const hdrg_kernels_t *kernels, const hdrg_source_t *src,
                                          const hdrg_analysis_t *analysis, uint8_t shift,
                                          const hdrg_params_t *params,
                                          hdrg_plan_moments_fn_t moments, void *moments_arg,
                                          hdrg_plan_t *out);
/* the fixed plan of params for a width x height source (it holds no memory) */
extern void hdrg_plan_fixed(const hdrg_params_t *params, uint32_t width, uint32_t height,
                            hdrg_plan_t *out);
/* the boost of a fixed plan: each lane's share from its La (lo, hi: the ramp's ends) */
extern void hdrg_plan_fixed_ramp(float *lo, float *hi);
extern void hdrg_plan_free(const hdrg_allocator_t *alloc, hdrg_plan_t *plan);

#endif /* HDRG_CORE_PLAN_H */
