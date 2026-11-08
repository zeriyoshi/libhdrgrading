/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "plan.h"

#include <math.h>
#include <string.h>

#include "hdrgrading/foundation/fmath.h"
#include "hdrgrading/foundation/math.h"

/*
 * The diffuse level rises with the key (log-average La) from KEY_DARK (a
 * dark or low-key frame keeps the SDR level: its lights carry the HDR) to
 * KEY_LIT (a normally exposed frame takes the full diffuse_max). Kodak's 24
 * photographs have keys of 0.05 to 0.35, most of them 0.09 to 0.22
 *.
 */
#define HDRG_KEY_DARK      0.05f
#define HDRG_KEY_LIT       0.15f
#define HDRG_FIXED_DIFFUSE 0.5f       /* a fixed plan's share of the diffuse range */
#define HDRG_FIXED_LO      -0.321928f /* a fixed plan's boost ramp, log2 La: 0.8 (near white) ... */
#define HDRG_FIXED_HI      0.0f       /* ... to white */
#define HDRG_PLAN_BLOCK    HDRG_KERNEL_BLOCK /* pixels per boost kernel call */
#define HDRG_PLAN_WEIGHT   0.000001f         /* a grid bin with less weight holds no boost */
#define HDRG_CLARITY_DIVISOR 20u             /* clarity base radius: longer side / 20 ... */
#define HDRG_CLARITY_MIN     4u              /* ... clamped to [4, 200] source pixels */
#define HDRG_CLARITY_MAX     200u
#define HDRG_CORE_MIN        (0.5f / 255.0f) /* Y' noise below this (8-bit rounding): no coring */
#define HDRG_CORE_SIGMAS     3.0f            /* deviations within this many sigmas are cored */
#define HDRG_CORE_SPREAD     0.94280904f     /* sqrt(8 / 9): a pixel about its 3x3 mean */

typedef struct {
    const hdrg_source_t *src;
    const hdrg_analysis_t *analysis;
    const hdrg_kernels_t *kernels; /* the SIMD tier's boost kernel */
    hdrg_boost_args_t args;
    double *sums; /* per cell: HDRG_PLAN_MOMENTS */
    float *grid;  /* per cell and bin: weight, weight x boost */
    uint32_t grid_w, cell;
    uint8_t light_shift; /* log2 of the source pixels an analysis cell spans */
} hdrg_moment_job_t;

/* the grid's per-cell passes after the moments, a job row = a row of cells */
typedef struct {
    const hdrg_params_t *params;
    const double *sums;
    const float *src; /* the blur's input: weight, weight x boost per cell and bin */
    float *dst;       /* its output */
    hdrg_plan_t *plan;
    float *top; /* per cell: its largest boost share */
    bool down;  /* the blur goes down the rows (else across) */
} hdrg_grid_job_t;

/* the light of the analysis cell under source pixel (x, y): its boost and its ramp */
static inline void hdrg_plan_light(const hdrg_analysis_t *analysis, uint8_t shift, uint32_t x,
                                   uint32_t y, float *boost, float *lo, float *hi)
{
    const hdrg_plane_t *light = &analysis->light;
    size_t i;

    i = (size_t)HDRG_MIN(y >> shift, light->height - 1u) * light->width +
        HDRG_MIN(x >> shift, light->width - 1u);
    *boost = light->data[i];
    *lo = analysis->light_lo.data[i];
    *hi = analysis->light_hi.data[i];
}

/*
 * Accumulates the per-cell moments and the boost grid (pass B); one job row
 * = one row of cells. Each pixel's boost share comes from the light of its
 * analysis cell through the tier's boost kernel; the sums take the pixels in
 * order.
 */
static void hdrg_moment_rows(uint32_t cell_row_begin, uint32_t cell_row_end, void *arg)
{
    const hdrg_moment_job_t *job = (const hdrg_moment_job_t *)arg;
    const hdrg_source_t *src = job->src;
    const uint8_t *row;
    uint32_t x, x0, y, y_end, cy, cell, i, j, end, n, bin;
    uint8_t k;
    double *s, m[HDRG_PLAN_MOMENTS];
    float pixel[3], lum_y[HDRG_PLAN_BLOCK], luma[HDRG_PLAN_BLOCK], boost[HDRG_PLAN_BLOCK],
        lo[HDRG_PLAN_BLOCK], hi[HDRG_PLAN_BLOCK], h[HDRG_PLAN_BLOCK], log_luma[HDRG_PLAN_BLOCK], *g,
        t;

    for (cy = cell_row_begin; cy < cell_row_end; cy++) {
        y_end = HDRG_MIN((cy + 1) * job->cell, src->height);
        for (y = cy * job->cell; y < y_end; y++) {
            row = hdrg_source_row(src, y);

            for (x0 = 0; x0 < src->width; x0 += HDRG_PLAN_BLOCK) {
                n = HDRG_MIN(src->width - x0, HDRG_PLAN_BLOCK);

                for (i = 0; i < n; i++) {
                    x = x0 + i;
                    hdrg_source_rgb(src, row, x, pixel);
                    lum_y[i] = hdrg_rgb_y(src->coeffs, pixel);
                    luma[i] = hdrg_rgb_la(lum_y[i], pixel);
                    hdrg_plan_light(job->analysis, job->light_shift, x, y, &boost[i], &lo[i],
                                    &hi[i]);
                }

                /* the kernel computes every lane: the ones past the row's end get zeros */
                for (i = n; i < HDRG_PLAN_BLOCK; i++) {
                    luma[i] = 0.0f;
                    boost[i] = 0.0f;
                    lo[i] = -1.0f;
                    hi[i] = 0.0f;
                }

                job->kernels->boost(&job->args, luma, boost, lo, hi, h, log_luma);

                /*
                 * the sums of each cell in registers, the pixels in order:
                 * the same additions as one pixel at a time through memory
                 */
                for (i = 0; i < n; i = end) {
                    cell = cy * job->grid_w + (x0 + i) / job->cell;
                    end = HDRG_MIN(n, ((x0 + i) / job->cell + 1u) * job->cell - x0);
                    s = job->sums + (size_t)cell * HDRG_PLAN_MOMENTS;
                    g = job->grid + (size_t)cell * HDRG_PLAN_BINS * 2u;

                    for (k = 0; k < HDRG_PLAN_MOMENTS; k++) {
                        m[k] = s[k];
                    }

                    for (j = i; j < end; j++) {
                        m[0] += 1.0;
                        m[1] += (double)luma[j];
                        m[2] += (double)lum_y[j];
                        m[3] += (double)lum_y[j] * (double)h[j];

                        m[4] += (double)log_luma[j];

                        bin = hdrg_plan_bin(luma[j], &t);
                        g[bin * 2u] += 1.0f - t;
                        g[bin * 2u + 1u] += (1.0f - t) * h[j];
                        g[bin * 2u + 2u] += t;
                        g[bin * 2u + 3u] += t * h[j];
                    }

                    for (k = 0; k < HDRG_PLAN_MOMENTS; k++) {
                        s[k] = m[k];
                    }
                }
            }
        }
    }
}

/*
 * A [1 2 1] / 4 pass over the grid (edges repeated): across the cells of a
 * row (step 1) or down the rows (step grid_w), every value of a cell alike
 */
static void hdrg_grid_blur(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_grid_job_t *job = (const hdrg_grid_job_t *)arg;
    const uint32_t grid_w = job->plan->grid_w, grid_h = job->plan->grid_h;
    const size_t stride = (size_t)HDRG_PLAN_BINS * 2u;
    const float *c, *l, *r;
    uint32_t x, y, b, xm, xp, ym, yp;

    for (y = row_begin; y < row_end; y++) {
        for (x = 0; x < grid_w; x++) {
            xm = (!job->down && x > 0) ? x - 1u : x;
            xp = (!job->down && x + 1u < grid_w) ? x + 1u : x;
            ym = (job->down && y > 0) ? y - 1u : y;
            yp = (job->down && y + 1u < grid_h) ? y + 1u : y;
            c = job->src + ((size_t)y * grid_w + x) * stride;
            l = job->src + ((size_t)ym * grid_w + xm) * stride;
            r = job->src + ((size_t)yp * grid_w + xp) * stride;

            for (b = 0; b < stride; b++) {
                job->dst[((size_t)y * grid_w + x) * stride + b] =
                    (l[b] + 2.0f * c[b] + r[b]) * 0.25f;
            }
        }
    }
}

/* the blurred grid normalized into the boost shares, and each cell's largest */
static void hdrg_grid_normalize(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_grid_job_t *job = (const hdrg_grid_job_t *)arg;
    const float *grid = job->src;
    uint32_t b;
    size_t i;
    double w;
    float *boost = job->plan->boost;

    for (i = (size_t)row_begin * job->plan->grid_w; i < (size_t)row_end * job->plan->grid_w; i++) {
        job->top[i] = 0.0f;

        for (b = 0; b < HDRG_PLAN_BINS; b++) {
            w = (double)grid[(i * HDRG_PLAN_BINS + b) * 2u];
            boost[i * HDRG_PLAN_BINS + b] =
                (w > HDRG_PLAN_WEIGHT)
                    ? HDRG_CLAMP((float)((double)grid[(i * HDRG_PLAN_BINS + b) * 2u + 1u] / w),
                                 0.0f, 1.0f)
                    : 0.0f;
            job->top[i] = HDRG_MAX(job->top[i], boost[i * HDRG_PLAN_BINS + b]);
        }
    }
}

/*
 * APL statistics per cell: mean SDR luminance, contrast lift at the mean La,
 * the luminance-weighted boost share. The render estimates the rendered
 * average luminance from these for any display.
 */
static void hdrg_grid_apl(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_grid_job_t *job = (const hdrg_grid_job_t *)arg;
    const double *sums = job->sums;
    hdrg_plan_t *plan = job->plan;
    size_t i;
    float mean_l;

    for (i = (size_t)row_begin * plan->grid_w; i < (size_t)row_end * plan->grid_w; i++) {
        mean_l = (float)(sums[i * HDRG_PLAN_MOMENTS + 1] / sums[i * HDRG_PLAN_MOMENTS]);
        plan->apl_y[i] = (float)(sums[i * HDRG_PLAN_MOMENTS + 2] / sums[i * HDRG_PLAN_MOMENTS]);
        plan->apl_lift[i] =
            (hdrg_math_log2f(HDRG_MAX(mean_l, HDRG_LUMA_FLOOR)) - HDRG_GRADE_PIVOT) *
            (job->params->contrast - 1.0f);
        plan->apl_boost[i] =
            (sums[i * HDRG_PLAN_MOMENTS + 2] > 0.0)
                ? (float)(sums[i * HDRG_PLAN_MOMENTS + 3] / sums[i * HDRG_PLAN_MOMENTS + 2])
                : 0.0f;
    }
}

/*
 * The coring table: the noise level function (Y' codes) in La units through
 * the slope of the source's green EOTF at each level; false without noise
 */
static bool hdrg_plan_coring_table(const hdrg_source_t *src, const hdrg_noise_t *noise,
                                   float table[HDRG_PLAN_CORING])
{
    const uint32_t top = src->entries - 1u, step = HDRG_MAX(top / 255u, 1u);
    const float *lut = src->lut + src->entries;
    uint32_t i, lo, hi, mid, c0, c1;
    float la, slope, sigma;

    if (noise->sigma < HDRG_CORE_MIN) {
        return false;
    }

    for (i = 0; i < HDRG_PLAN_CORING; i++) {
        la = (float)i / (float)(HDRG_PLAN_CORING - 1u);
        lo = 0;
        hi = top;

        /* the first code at or above la (the EOTF is monotonic) */
        while (lo < hi) {
            mid = lo + (hi - lo) / 2u;

            if (lut[mid] < la) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }

        c0 = (lo > step) ? lo - step : 0u;
        c1 = HDRG_MIN(lo + step, top);
        slope = (lut[c1] - lut[c0]) * (float)top / (float)(c1 - c0);
        sigma = hdrg_noise_at(noise, (float)lo / (float)top) * slope * HDRG_CORE_SIGMAS *
                HDRG_CORE_SPREAD;
        table[i] = sigma * sigma;
    }

    return true;
}

extern float hdrg_plan_diffuse(const hdrg_params_t *params, float key)
{
    float t;

    t = HDRG_CLAMP((key - HDRG_KEY_DARK) / (HDRG_KEY_LIT - HDRG_KEY_DARK), 0.0f, 1.0f);

    return hdrg_math_log2f(HDRG_MAX(params->diffuse_max, 1.0f)) * t * t * (3.0f - 2.0f * t);
}

extern hdrg_status_t hdrg_plan_build(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                     const hdrg_kernels_t *kernels, const hdrg_source_t *src,
                                     const hdrg_analysis_t *analysis, uint8_t shift,
                                     const hdrg_params_t *params, hdrg_plan_t *out)
{
    return hdrg_plan_build_with(alloc, exec, kernels, src, analysis, shift, params, NULL, NULL,
                                out);
}

extern hdrg_status_t hdrg_plan_build_with(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                          const hdrg_kernels_t *kernels, const hdrg_source_t *src,
                                          const hdrg_analysis_t *analysis, uint8_t shift,
                                          const hdrg_params_t *params,
                                          hdrg_plan_moments_fn_t moments, void *moments_arg,
                                          hdrg_plan_t *out)
{
    const bool clarity = params->clarity > 0.0f;
    hdrg_moment_job_t moment_job;
    hdrg_grid_job_t grid_job;
    hdrg_status_t status;
    uint32_t radius, x, y, right;
    size_t cells, i, below;
    double *sums;
    float *grid, *tmp;

    memset(out, 0, sizeof(*out));

    out->cell = HDRG_PLAN_CELL >> shift;
    out->width = src->width;
    out->height = src->height;
    out->grid_w = (src->width + out->cell - 1) / out->cell;
    out->grid_h = (src->height + out->cell - 1) / out->cell;
    out->cored = hdrg_plan_coring_table(src, &analysis->noise, out->coring);
    out->diffuse = hdrg_plan_diffuse(params, analysis->key_luma);
    cells = (size_t)out->grid_w * out->grid_h;

    sums = (double *)hdrg_mem_alloc(alloc, cells * HDRG_PLAN_MOMENTS * sizeof(double));
    grid = (float *)hdrg_mem_alloc(alloc, cells * HDRG_PLAN_BINS * 4u * sizeof(float));
    tmp = (float *)hdrg_mem_alloc(alloc, cells * 2u * sizeof(float));
    out->boost = (float *)hdrg_mem_alloc(alloc, cells * (HDRG_PLAN_BINS + (clarity ? 5u : 4u)) *
                                                    sizeof(float));
    if (sums == NULL || grid == NULL || tmp == NULL || out->boost == NULL) {
        status = HDRG_ERR_NOMEM;

        goto cleanup;
    }

    out->boost_near = out->boost + cells * HDRG_PLAN_BINS;
    out->apl_y = out->boost_near + cells;
    out->apl_lift = out->apl_y + cells;
    out->apl_boost = out->apl_lift + cells;
    out->base = clarity ? out->apl_boost + cells : NULL;

    moment_job.src = src;
    moment_job.analysis = analysis;
    moment_job.kernels = (kernels != NULL) ? kernels : &hdrg_kernels_scalar;
    moment_job.args.floor = HDRG_LUMA_FLOOR;
    moment_job.sums = sums;
    moment_job.grid = grid;
    moment_job.grid_w = out->grid_w;
    moment_job.cell = out->cell;
    moment_job.light_shift = (uint8_t)(analysis->cell_shift - shift);

    if (moments != NULL) {
        status = moments(moments_arg, src, analysis, moment_job.light_shift, out->cell, out->grid_w,
                         out->grid_h, sums, grid);
        if (status != HDRG_OK) {
            goto cleanup;
        }
    } else {
        /*
         * the CPU's sums add up from zero (the GPU's come written whole); the grid's second half
         * is the blur's output, written whole
         */
        memset(sums, 0, cells * HDRG_PLAN_MOMENTS * sizeof(double));
        memset(grid, 0, cells * HDRG_PLAN_BINS * 2u * sizeof(float));
        hdrg_exec_rows(exec, out->grid_h, hdrg_moment_rows, &moment_job);
    }

    /* the grid blurred over the neighboring cells (weights and boosts alike), then normalized */
    grid_job.params = params;
    grid_job.sums = sums;
    grid_job.plan = out;
    grid_job.top = tmp;
    grid_job.src = grid;
    grid_job.dst = grid + cells * HDRG_PLAN_BINS * 2u;
    grid_job.down = false;

    hdrg_exec_rows(exec, out->grid_h, hdrg_grid_blur, &grid_job);

    grid_job.src = grid + cells * HDRG_PLAN_BINS * 2u;
    grid_job.dst = grid;
    grid_job.down = true;

    hdrg_exec_rows(exec, out->grid_h, hdrg_grid_blur, &grid_job);

    grid_job.src = grid;

    hdrg_exec_rows(exec, out->grid_h, hdrg_grid_normalize, &grid_job);

    for (i = 0; i < cells; i++) {
        out->boost_max = HDRG_MAX(out->boost_max, tmp[i]);
    }

    /* the largest share over the 2 x 2 cells a pixel interpolates from each */
    for (y = 0; y < out->grid_h; y++) {
        for (x = 0; x < out->grid_w; x++) {
            right = HDRG_MIN(x + 1u, out->grid_w - 1u);
            below = (size_t)HDRG_MIN(y + 1u, out->grid_h - 1u) * out->grid_w;
            out->boost_near[(size_t)y * out->grid_w + x] = HDRG_MAX(
                HDRG_MAX(tmp[(size_t)y * out->grid_w + x], tmp[(size_t)y * out->grid_w + right]),
                HDRG_MAX(tmp[below + x], tmp[below + right]));
        }
    }

    /*
     * Clarity base: the cell means of log2 La, box-blurred over a radius of
     * a twentieth of the longer side
     */
    if (clarity) {
        for (i = 0; i < cells; i++) {
            tmp[i] = (float)(sums[i * HDRG_PLAN_MOMENTS + 4] / sums[i * HDRG_PLAN_MOMENTS]);
        }

        radius = HDRG_MAX(src->width, src->height) / HDRG_CLARITY_DIVISOR;
        radius = HDRG_CLAMP(radius, HDRG_CLARITY_MIN, HDRG_CLARITY_MAX);
        radius = HDRG_MAX((radius + out->cell / 2) / out->cell, 1u);

        hdrg_box_blur_plane(exec, tmp, tmp + cells, out->base, out->grid_w, out->grid_h, radius);
    }

    hdrg_exec_rows(exec, out->grid_h, hdrg_grid_apl, &grid_job);

    status = HDRG_OK;

cleanup:
    hdrg_mem_free(alloc, sums);
    hdrg_mem_free(alloc, grid);
    hdrg_mem_free(alloc, tmp);

    if (status != HDRG_OK) {
        hdrg_plan_free(alloc, out);
    }

    return status;
}

extern void hdrg_plan_fixed(const hdrg_params_t *params, uint32_t width, uint32_t height,
                            hdrg_plan_t *out)
{
    memset(out, 0, sizeof(*out));

    out->width = width;
    out->height = height;
    out->grid_w = 1;
    out->grid_h = 1;
    out->cell = HDRG_MAX(width, height);
    out->fixed = true;
    out->diffuse = hdrg_math_log2f(HDRG_MAX(params->diffuse_max, 1.0f)) * HDRG_FIXED_DIFFUSE;
    out->boost_max = 1.0f;
}

extern void hdrg_plan_fixed_ramp(float *lo, float *hi)
{
    *lo = HDRG_FIXED_LO;
    *hi = HDRG_FIXED_HI;
}

extern void hdrg_plan_free(const hdrg_allocator_t *alloc, hdrg_plan_t *plan)
{
    if (plan == NULL) {
        return;
    }

    hdrg_mem_free(alloc, plan->boost);

    memset(plan, 0, sizeof(*plan));
}
