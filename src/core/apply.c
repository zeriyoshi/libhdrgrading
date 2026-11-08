/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "apply.h"

#include <string.h>

#include "hdrgrading/foundation/fmath.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/math.h"

/* dither key of the counter-based hash for rendition pixels */
#define HDRG_KEY_RENDER     UINT64_C(0x484452475F585047)
#define HDRG_SHOULDER_START 0.75f             /* soft shoulder onset, fraction of peak */
#define HDRG_APPLY_BLOCK    HDRG_KERNEL_BLOCK /* pixels per stage of the row loop */
#define HDRG_LN2            0.69314718f

typedef struct {
    const hdrg_apply_t *apply;
    uint8_t *dst;
    uint32_t y0;
    uint32_t lo, hi; /* source rows readable */
} hdrg_apply_job_t;

/* a worker's La and max(R, G, B) of the rows above, at and below the output row (coring) */
typedef struct {
    float *rows[3];
    float *peaks[3];
    float *sums; /* the three rows' column sums */
    float *peak_sums;
} hdrg_apply_ring_t;

/* one block, a row of lanes per channel (the kernels' layout) */
typedef struct {
    uint32_t codes[3][HDRG_APPLY_BLOCK];
    float luma[HDRG_APPLY_BLOCK];  /* analysis luminance */
    float gain[HDRG_APPLY_BLOCK];  /* plan gain, stops */
    float base[HDRG_APPLY_BLOCK];  /* clarity base */
    float guide[HDRG_APPLY_BLOCK]; /* the desaturation's peak factor: 3x3 mean max over max */
    float ratio[HDRG_APPLY_BLOCK];
    float rgb[3][HDRG_APPLY_BLOCK];
    float linear[3][HDRG_APPLY_BLOCK]; /* a fixed plan: source linear RGB, the law's input */
    float out[3][HDRG_APPLY_BLOCK];    /* OETF input */
    float lr[HDRG_APPLY_BLOCK];        /* two-layer modes: log2 tone-map ratio */
} hdrg_apply_block_t;

/*
 * Stage 1 of a block: code values -> source linear luminance, the plan's
 * boost share (its bilateral grid sliced at the pixel's position and La) and
 * clarity base at each pixel, the gain diffuse + h * span, then the grading
 * ratio (the tier's grade kernel). Grading happens on log2 luminance and is
 * applied as a per-pixel ratio on RGB, so hue and saturation ratios are
 * structurally preserved:
 *
 *   t' = pivot + (t - pivot) * contrast   (S-slope around mid-gray)
 *      + clarity * (t - base(x, y))       (local contrast, soft-limited)
 *      + g(x, y)                          (gain, stops)
 */
static inline void hdrg_apply_gain(const hdrg_apply_t *apply, const uint8_t *row, uint32_t x0,
                                   uint32_t n, uint32_t y, const hdrg_apply_ring_t *ring,
                                   hdrg_apply_block_t *blk)
{
    const hdrg_plan_t *plan = apply->plan;
    const uint32_t width = apply->src->width;
    hdrg_grid_pos_t pos;
    uint32_t i, x, codes[3];
    uint8_t c;
    float rgb[3], la, m, d, s, cored, peak;

    for (i = 0; i < n; i++) {
        hdrg_source_codes(apply->src, row, x0 + i, codes);

        for (c = 0; c < 3; c++) {
            blk->codes[c][i] = codes[c];
        }

        if (ring != NULL) {
            blk->luma[i] = ring->rows[1][x0 + i];
        } else {
            hdrg_source_linear(apply->src, codes, rgb);
            blk->luma[i] = hdrg_rgb_la(hdrg_rgb_y(apply->src->coeffs, rgb), rgb);
        }
    }

    hdrg_grid_pos(apply->grid_scale_x, apply->grid_scale_y, plan->grid_w, plan->grid_h, 0, y, &pos);

    for (i = 0; i < n; i++) {
        x = x0 + i;
        pos.x0 = apply->grid_x[x];
        pos.x1 = HDRG_MIN(pos.x0 + 1u, plan->grid_w - 1u);
        pos.tx = apply->grid_tx[x];
        blk->base[i] = (plan->base != NULL) ? hdrg_grid_lerp(plan->base, plan->grid_w, &pos) : 0.0f;
        blk->guide[i] = 1.0f;
        la = blk->luma[i];

        /*
         * coring: La toward its 3x3 mean (edge columns repeated) where it
         * deviates by the noise level; the clarity detail follows it
         * (log2(La / cored) to first order). The highlight desaturation is
         * decided on the 3x3 mean of the max channel: its noise (and the
         * chroma noise in max / La) would move the low channels a lot
         */
        if (ring != NULL) {
            m = (ring->sums[(x > 0) ? x - 1u : 0u] + ring->sums[x] +
                 ring->sums[HDRG_MIN(x + 1u, width - 1u)]) /
                9.0f;
            d = la - m;
            s = d * d + hdrg_plan_coring(plan, m);
            cored = (s > 0.0f) ? m + d * (d * d / s) : la;
            blk->base[i] += (la - cored) / (HDRG_MAX(cored, HDRG_LUMA_FLOOR) * HDRG_LN2);
            la = cored;
            peak = ring->peaks[1][x];
            m = (ring->peak_sums[(x > 0) ? x - 1u : 0u] + ring->peak_sums[x] +
                 ring->peak_sums[HDRG_MIN(x + 1u, width - 1u)]) /
                9.0f;
            blk->guide[i] = (peak > 0.0f) ? m / peak : 1.0f;
        }

        blk->gain[i] = apply->diffuse;

        /* away from the lights the 2 x 2 cells hold no boost (exactly 0 sliced) */
        if (plan->boost != NULL &&
            plan->boost_near[(size_t)pos.y0 * plan->grid_w + pos.x0] > 0.0f) {
            blk->gain[i] += hdrg_boost_at(plan->boost, plan->grid_w, &pos, la) * apply->span;
        }
    }

    /* the kernels compute every lane: the ones past the row's end get zeros */
    for (i = n; i < HDRG_APPLY_BLOCK; i++) {
        blk->luma[i] = 0.0f;
        blk->gain[i] = 0.0f;
        blk->base[i] = 0.0f;
        blk->guide[i] = 1.0f;

        for (c = 0; c < 3; c++) {
            blk->codes[c][i] = 0;
        }
    }

    apply->kernels->grade(&apply->grade, blk->luma, blk->gain, blk->base, blk->ratio);
}

/*
 * stage 1 under a fixed plan: the diffuse level and each pixel's boost from
 * its La alone (the tier's boost kernel on the fixed ramp)
 */
static inline void hdrg_apply_fixed_gain(const hdrg_apply_t *apply, const uint8_t *row, uint32_t x0,
                                         uint32_t n, hdrg_apply_block_t *blk)
{
    const hdrg_boost_args_t args = {HDRG_LUMA_FLOOR};
    uint32_t i, codes[3];
    uint8_t c;
    float rgb[3], boost[HDRG_APPLY_BLOCK], lo[HDRG_APPLY_BLOCK], hi[HDRG_APPLY_BLOCK],
        h[HDRG_APPLY_BLOCK];

    for (i = 0; i < HDRG_APPLY_BLOCK; i++) {
        memset(codes, 0, sizeof(codes));
        memset(rgb, 0, sizeof(rgb));

        if (i < n) {
            hdrg_source_codes(apply->src, row, x0 + i, codes);
            hdrg_source_linear(apply->src, codes, rgb);
        }

        for (c = 0; c < 3; c++) {
            blk->codes[c][i] = codes[c];
        }

        blk->luma[i] = (i < n) ? hdrg_rgb_la(hdrg_rgb_y(apply->src->coeffs, rgb), rgb) : 0.0f;
        blk->guide[i] = 1.0f;
        blk->base[i] = 0.0f;
        boost[i] = 1.0f;
        hdrg_plan_fixed_ramp(&lo[i], &hi[i]);
    }

    apply->kernels->boost(&args, blk->luma, boost, lo, hi, h, blk->base);

    for (i = 0; i < HDRG_APPLY_BLOCK; i++) {
        blk->gain[i] = apply->diffuse + h[i] * apply->span;
    }

    apply->kernels->grade(&apply->grade, blk->luma, blk->gain, blk->base, blk->ratio);
}

/*
 * Two-layer rendition, the tier's layer kernel: grade in
 * the rendition gamut, then tone-map with a per-pixel scalar (knee on the
 * max channel, the same scale applied to all channels) so the monochrome
 * gain map reconstructs the HDR target exactly and hue / saturation are
 * preserved. GAINMAP emits the SDR base; HYBRID emits the pre-tone-map HDR
 * rendition PQ-encoded (Display P3), whose SDR rendition is recovered by
 * applying -log2 ratio. The log2 ratios go to the gain-map cells in pixel
 * order.
 */
static inline void hdrg_apply_layer_block(const hdrg_apply_t *apply, uint32_t x0, uint32_t y,
                                          uint32_t n, hdrg_apply_block_t *blk)
{
    uint32_t i, chunk;
    float *gm_row;

    apply->kernels->layer(&apply->layer, (const float (*)[HDRG_KERNEL_BLOCK])blk->rgb, blk->guide,
                          blk->out, blk->lr);

    gm_row = apply->gm_sum + (size_t)(y >> apply->gm_shift) * apply->gm_w;
    chunk = y / HDRG_EXEC_GRAIN_ROWS;

    for (i = 0; i < n; i++) {
        gm_row[(x0 + i) >> apply->gm_shift] += blk->lr[i];
        apply->chunk_max[chunk] = HDRG_MAX(apply->chunk_max[chunk], blk->lr[i]);
    }
}

/* last stage, the tier's pack kernel: OETF (out holds nits for PQ, [0, 1] linear for sRGB), dither,
 * pack */
static inline void hdrg_apply_pack(const hdrg_apply_t *apply, uint32_t x0, uint32_t row_hash,
                                   uint32_t n, bool pq, const hdrg_apply_block_t *blk, uint8_t *dst)
{
    if (apply->sample_bytes == 2) {
        (pq ? apply->kernels->pack_pq16
            : apply->kernels->pack_srgb16)((const float (*)[HDRG_KERNEL_BLOCK])blk->out, n,
                                           (uint16_t *)(void *)dst + (size_t)x0 * 3u);

        return;
    }

    (pq ? apply->kernels->pack_pq
        : apply->kernels->pack_srgb)((const float (*)[HDRG_KERNEL_BLOCK])blk->out, n, x0, row_hash,
                                     apply->dither, dst + (size_t)x0 * 3u);
}

/*
 * single-layer PQ (and the hybrid's primary): the light of the block's
 * pixels (from x0 of row y, nits) into its chunk (in pixel order) when
 * measured, and its hdrg_lightstat_t when kept
 */
static inline void hdrg_apply_light(const hdrg_apply_t *apply, uint32_t x0, uint32_t y, uint32_t n,
                                    const hdrg_apply_block_t *blk)
{
    const uint32_t chunk = y / HDRG_EXEC_GRAIN_ROWS, entries = apply->src->entries;
    const float *lut = apply->src->lut, *k = apply->light_matrix;
    hdrg_lightstat_t *light;
    uint32_t i, c, bin;
    int32_t q;
    float m, s, v, lin[3];

    for (i = 0; apply->chunk_cll != NULL && i < n; i++) {
        m = HDRG_MAX(blk->out[0][i], HDRG_MAX(blk->out[1][i], blk->out[2][i]));
        apply->chunk_cll[chunk] = HDRG_MAX(apply->chunk_cll[chunk], m);
        apply->chunk_fall[chunk] += (double)m;
    }

    if (apply->chunk_light == NULL) {
        return;
    }

    light = &apply->chunk_light[chunk];

    for (c = 0; c < 3u; c++) {
        for (i = 0; i < n; i++) {
            light->channel[c] = HDRG_MAX(light->channel[c], blk->out[c][i]);
        }
    }

    /* the sampled pixels: the bin, and the SDR's max(R, G, B) in the rendition gamut over it */
    for (i = (HDRG_LIGHT_STEP - x0 % HDRG_LIGHT_STEP) % HDRG_LIGHT_STEP;
         y % HDRG_LIGHT_STEP == 0 && i < n; i += HDRG_LIGHT_STEP) {
        m = HDRG_MAX(blk->out[0][i], HDRG_MAX(blk->out[1][i], blk->out[2][i]));
        bin = hdrg_lightstat_bin(m);
        light->count[bin]++;
        s = 0.0f;

        for (c = 0; c < 3u; c++) {
            lin[c] = lut[c * entries + blk->codes[c][i]];
        }

        for (c = 0; c < 3u; c++) {
            v = k[c * 3u] * lin[0] + k[c * 3u + 1u] * lin[1] + k[c * 3u + 2u] * lin[2];
            s = HDRG_MAX(s, v);
        }

        if (!(m > 0.0f) || !(s > 0.0f)) {
            continue;
        }

        v = hdrg_fmath_log2(s * apply->light_white / m) * HDRG_LIGHT_Q;
        q = (int32_t)((v >= 0.0f) ? v + 0.5f : v - 0.5f);
        q = HDRG_MIN(HDRG_MAX(q, -HDRG_LIGHT_SPAN), HDRG_LIGHT_SPAN - 1);
        light->samples[bin]++;
        light->ratio[bin] += (uint32_t)(q + HDRG_LIGHT_SPAN);
        light->place[bin] += hdrg_lightstat_place(m);
    }
}

/* the analysis luminance and the linear max(R, G, B) of source row y */
static inline void hdrg_apply_la_row(const hdrg_source_t *src, uint32_t y, float *la, float *peak)
{
    const uint8_t *row = hdrg_source_row(src, y);
    uint32_t x;
    float rgb[3];

    for (x = 0; x < src->width; x++) {
        hdrg_source_rgb(src, row, x, rgb);
        la[x] = hdrg_rgb_la(hdrg_rgb_y(src->coeffs, rgb), rgb);
        peak[x] = HDRG_MAX(rgb[0], HDRG_MAX(rgb[1], rgb[2]));
    }
}

/*
 * The ring at output row y, the first of a worker's run or the next one:
 * the rows above and below it (the edge rows of [lo, hi) repeated) and the
 * column sums
 */
static inline void hdrg_apply_ring(const hdrg_source_t *src, hdrg_apply_ring_t *ring, uint32_t y,
                                   bool first, uint32_t lo, uint32_t hi)
{
    uint32_t x;
    float *spare;

    if (first) {
        hdrg_apply_la_row(src, (y > lo) ? y - 1u : y, ring->rows[0], ring->peaks[0]);
        hdrg_apply_la_row(src, y, ring->rows[1], ring->peaks[1]);
    } else {
        spare = ring->rows[0];
        ring->rows[0] = ring->rows[1];
        ring->rows[1] = ring->rows[2];
        ring->rows[2] = spare;
        spare = ring->peaks[0];
        ring->peaks[0] = ring->peaks[1];
        ring->peaks[1] = ring->peaks[2];
        ring->peaks[2] = spare;
    }

    hdrg_apply_la_row(src, (y + 1u < hi) ? y + 1u : y, ring->rows[2], ring->peaks[2]);

    for (x = 0; x < src->width; x++) {
        ring->sums[x] = ring->rows[0][x] + ring->rows[1][x] + ring->rows[2][x];
        ring->peak_sums[x] = ring->peaks[0][x] + ring->peaks[1][x] + ring->peaks[2][x];
    }
}

/*
 * A row runs in blocks of HDRG_APPLY_BLOCK pixels, one stage at a time:
 * each stage is a long serial chain (log2 / exp2 / OETF calls), and
 * independent pixels inside a stage keep the core busy. The per-pixel
 * arithmetic does not depend on the blocking.
 */
static void hdrg_apply_band_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_apply_job_t *job = (const hdrg_apply_job_t *)arg;
    const hdrg_apply_t *apply = job->apply;
    const uint8_t *row;
    const bool pq_out = apply->mode != HDRG_OUTPUT_GAINMAP,
               cored = apply->plan->cored && !apply->plan->fixed;
    hdrg_apply_block_t blk;
    hdrg_apply_ring_t ring;
    uint32_t width, x0, y, r, n, row_hash;
    uint8_t *dst;
    int32_t *grain_rows;

    width = apply->src->width;
    grain_rows =
        (apply->grain != NULL && pq_out && apply->grain_rows != NULL)
            ? apply->grain_rows +
                  (size_t)worker * ((size_t)(HDRG_GRAIN_ROWS + 1u) * width + 2u * HDRG_GRAIN_RADIUS)
            : NULL;
    ring.rows[0] = apply->la_rows + (size_t)worker * 8u * width;
    ring.rows[1] = ring.rows[0] + width;
    ring.rows[2] = ring.rows[1] + width;
    ring.sums = ring.rows[2] + width;
    ring.peaks[0] = ring.sums + width;
    ring.peaks[1] = ring.peaks[0] + width;
    ring.peaks[2] = ring.peaks[1] + width;
    ring.peak_sums = ring.peaks[2] + width;

    for (r = row_begin; r < row_end; r++) {
        y = job->y0 + r;
        row = hdrg_source_row(apply->src, y);

        if (cored) {
            hdrg_apply_ring(apply->src, &ring, y, r == row_begin, job->lo, job->hi);
        }

        dst = job->dst + (size_t)r * width * 3u * apply->sample_bytes;
        row_hash = hdrg_hash_row(HDRG_KEY_RENDER, y);

        for (x0 = 0; x0 < width; x0 += HDRG_APPLY_BLOCK) {
            n = HDRG_MIN(width - x0, HDRG_APPLY_BLOCK);

            if (apply->plan->fixed) {
                hdrg_apply_fixed_gain(apply, row, x0, n, &blk);
            } else {
                hdrg_apply_gain(apply, row, x0, n, y, cored ? &ring : NULL, &blk);
            }

            /* stage 2: the color LUT (gamut mapping and saturation into the rendition gamut) */
            apply->kernels->lut(apply->lut, (const uint32_t (*)[HDRG_KERNEL_BLOCK])blk.codes,
                                blk.ratio, blk.rgb);

            if (apply->mode == HDRG_OUTPUT_PQ) {
                /* single-layer PQ: the C1 shoulder toward peak_nits (from 75% of it) */
                apply->kernels->shoulder(&apply->shoulder,
                                         (const float (*)[HDRG_KERNEL_BLOCK])blk.rgb, blk.guide,
                                         blk.out);

                if (apply->chunk_cll != NULL || apply->chunk_light != NULL) {
                    hdrg_apply_light(apply, x0, y, n, &blk);
                }
            } else {
                hdrg_apply_layer_block(apply, x0, y, n, &blk);

                /* the hybrid's PQ primary: its light (the HAGC's, the AVIF's CLLI) */
                if ((apply->chunk_cll != NULL || apply->chunk_light != NULL) &&
                    apply->mode == HDRG_OUTPUT_HYBRID) {
                    hdrg_apply_light(apply, x0, y, n, &blk);
                }
            }

            hdrg_apply_pack(apply, x0, row_hash, n, pq_out, &blk, dst);
        }

        /* RESYNTH: the frame's grain on the PQ row */
        if (grain_rows != NULL) {
            hdrg_grain_row(apply->grain, apply->src, row, y, r == row_begin, grain_rows,
                           grain_rows + (size_t)HDRG_GRAIN_ROWS * width, dst, apply->sample_bytes);
        }
    }
}

extern hdrg_status_t hdrg_apply_setup(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                      const hdrg_source_t *src, bool banded, const hdrg_lut_t *lut,
                                      const hdrg_kernels_t *kernels, const hdrg_params_t *params,
                                      const hdrg_encode_params_t *encode, hdrg_apply_t *out)
{
    memset(out, 0, sizeof(*out));

    out->workers = hdrg_exec_concurrency(exec);
    out->la_rows = (float *)hdrg_mem_calloc(alloc, (size_t)out->workers * 8u,
                                            (size_t)src->width * sizeof(float));
    out->grid_tx = (float *)hdrg_mem_alloc(alloc, (size_t)src->width * sizeof(float));
    out->grid_x = (uint32_t *)hdrg_mem_alloc(alloc, (size_t)src->width * sizeof(uint32_t));

    if (out->la_rows == NULL || out->grid_tx == NULL || out->grid_x == NULL) {
        hdrg_apply_free(alloc, out);

        return HDRG_ERR_NOMEM;
    }

    out->src = src;

    /* RESYNTH: the grain's rows from the start (no allocation on the way) */
    if (params->grain_policy == HDRG_GRAIN_RESYNTH &&
        hdrg_apply_grain_rows(alloc, out) != HDRG_OK) {
        hdrg_apply_free(alloc, out);

        return HDRG_ERR_NOMEM;
    }

    out->banded = banded;
    out->kernels = (kernels != NULL) ? kernels : &hdrg_kernels_scalar;
    out->lut = lut;

    out->grade.pivot = HDRG_GRADE_PIVOT;
    out->grade.floor = HDRG_LUMA_FLOOR;
    out->grade.contrast = params->contrast;
    out->grade.clarity = params->clarity;

    out->mode = encode->output_mode;
    out->sample_bytes = 1;
    out->dither = encode->dither;

    return HDRG_OK;
}

extern void hdrg_apply_bind(hdrg_apply_t *apply, const hdrg_plan_t *plan,
                            const hdrg_render_t *render, const hdrg_params_t *params)
{
    hdrg_grid_pos_t pos;
    uint32_t x;
    float coeffs[3];

    apply->plan = plan;
    apply->diffuse = render->diffuse;
    apply->span = render->span;
    apply->grid_scale_x = ((float)plan->width / (float)apply->src->width) / (float)plan->cell;
    apply->grid_scale_y = ((float)plan->height / (float)apply->src->height) / (float)plan->cell;
    apply->grade.clarity_on = plan->base != NULL;

    for (x = 0; x < apply->src->width; x++) {
        hdrg_grid_pos(apply->grid_scale_x, apply->grid_scale_y, plan->grid_w, plan->grid_h, x, 0,
                      &pos);
        apply->grid_x[x] = pos.x0;
        apply->grid_tx[x] = pos.tx;
    }

    hdrg_gamut_luma(render->gamut, coeffs);

    memcpy(apply->layer.coeffs, coeffs, sizeof(coeffs));
    apply->layer.desat = params->highlight_desat;
    apply->layer.start = 0.75f * render->dim * (render->peak_nits / render->sdr_white_nits);
    apply->layer.range = 0.25f * render->dim * (render->peak_nits / render->sdr_white_nits);
    apply->layer.knee = render->knee;
    apply->layer.white = render->sdr_white_nits;
    apply->layer.hybrid = render->mode == HDRG_OUTPUT_HYBRID;

    memcpy(apply->shoulder.coeffs, coeffs, sizeof(coeffs));
    apply->shoulder.desat = params->highlight_desat;
    apply->shoulder.knee = render->peak_nits * render->dim * HDRG_SHOULDER_START;
    apply->shoulder.range = render->peak_nits * render->dim - apply->shoulder.knee;
    apply->grade.pivot = HDRG_GRADE_PIVOT + hdrg_math_log2f(render->dim);
    apply->shoulder.white = render->sdr_white_nits;
}

extern hdrg_status_t hdrg_apply_grain_rows(const hdrg_allocator_t *alloc, hdrg_apply_t *apply)
{
    if (apply->grain_rows == NULL) {
        apply->grain_rows = (int32_t *)hdrg_mem_alloc(
            alloc,
            (size_t)apply->workers *
                ((size_t)(HDRG_GRAIN_ROWS + 1u) * apply->src->width + 2u * HDRG_GRAIN_RADIUS) *
                sizeof(int32_t));
    }

    return (apply->grain_rows != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
}

extern void hdrg_apply_free(const hdrg_allocator_t *alloc, hdrg_apply_t *apply)
{
    hdrg_mem_free(alloc, apply->la_rows);
    hdrg_mem_free(alloc, apply->grid_tx);
    hdrg_mem_free(alloc, apply->grid_x);
    hdrg_mem_free(alloc, apply->grain_rows);

    apply->la_rows = NULL;
    apply->grain_rows = NULL;
    apply->grid_tx = NULL;
    apply->grid_x = NULL;
}

extern void hdrg_apply_rows(const hdrg_exec_t *exec, const hdrg_apply_t *apply, uint32_t y0,
                            uint32_t rows, uint8_t *dst)
{
    hdrg_apply_job_t job;

    job.apply = apply;
    job.dst = dst;
    job.y0 = y0;
    job.lo = apply->banded ? y0 : 0u;
    job.hi = apply->banded ? y0 + rows : apply->src->height;

    hdrg_exec_worker_rows(exec, rows, hdrg_apply_band_rows, &job);
}
