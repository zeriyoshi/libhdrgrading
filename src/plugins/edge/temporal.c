/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <string.h>

#include "hdrgrading/plugins/blockme.h"
#include "edge.h"
#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_ST_ID        "hdrg.scale.temporal"
#define HDRG_ST_REACH     4u                        /* the window each way at most ... */
#define HDRG_ST_REFS      (2u * HDRG_ST_REACH)      /* ... its frames besides the job's */
#define HDRG_ST_SLOTS     (2u * HDRG_ST_REACH + 1u) /* the pyramids and lumas kept at most */
#define HDRG_ST_BLOCK     8u                        /* refined vectors: 8 x 8 input pixels */
#define HDRG_ST_LAMBDA    (HDRG_BME_LAMBDA / 16u)   /* the search's penalty: MCTF's */
#define HDRG_ST_ONE       256u                      /* weights in 1/256 */
#define HDRG_ST_UNIT      (1.0f / 65535.0f)
#define HDRG_ST_THRESHOLD 3.0 /* tau in 8-bit codes: a sample within it all, 3 times it none */
#define HDRG_ST_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define HDRG_ST_MAX(a, b) (((a) > (b)) ? (a) : (b))

/* the frames' weights by their distance from the job's (1/256: MCTF's, on to 4 frames) */
static const uint32_t hdrg_st_distance[HDRG_ST_REACH + 1u] = {256u, 218u, 146u, 85u, 44u};

/*
 * hdrg.scale.temporal: the library's multi-frame upscale of the SCALE
 * stage (HDRG_TEMPORAL_SR), deterministic (the same
 * whatever the threads): a window of the frames before and after the job's
 * (past / future, 2 each by default; those of its shot alone, job_shot).
 * For each of them the block motion estimation (hdrgrading/plugins/blockme.h,
 * phase 0: the job's frame's blocks found in it) refined for each 8 x 8
 * block of input pixels to 1/8 pixel, and the block's match (its mean
 * |difference| along the vector: all within tau, none from 3 tau), both on
 * the luma's 3 x 3 mean (bilinear): the aliasing a moved frame differs by
 * (what it adds) is finer than that, a wrong vector or an occlusion is
 * not. Each output pixel: the library's edge-adaptive kernel (edge.h) of
 * the job's frame at its place, and of each other frame at the place its
 * motion moves it to (the vectors between the blocks' centers bilinear),
 * the others weighted by their distance, their block's match and the
 * difference of the 3 x 3 means there (all within tau, none from 3 tau);
 * the mean. Without other frames (a still, the
 * stream's ends, a cut) the edge-adaptive upscale itself (hdrg.scale.edge's
 * bytes). Upscaling only; the same size copied.
 */
typedef struct {
    hdrg_bme_t bme;
    hdrg_bme_pyramid_t pyramids[HDRG_ST_SLOTS];
    uint32_t pyramid_index[HDRG_ST_SLOTS]; /* the frames the pyramids are of (UINT32_MAX: none) */
    float *lumas[HDRG_ST_SLOTS];           /* the frames' luma (edge.h's) ... */
    float *means[HDRG_ST_SLOTS];           /* ... and its 3 x 3 mean ... */
    uint32_t luma_index[HDRG_ST_SLOTS];    /* ... of these frames (UINT32_MAX: none) */
    int32_t *vectors[HDRG_ST_REFS];        /* each other frame's blocks' vectors (1/16 pixel) ... */
    uint16_t *weights[HDRG_ST_REFS];       /* ... and their match (1/256) */
    void *block;                           /* the lumas', the vectors' and the weights' memory */
    hdrg_edge_config_t config;
    uint32_t in_w, in_h, out_w, out_h, blocks_w, blocks_h; /* configure's (in_w 0: none yet) */
    uint32_t past, future;
    uint32_t slots; /* the window's frames (past + future + 1): the lumas and pyramids by index */
    uint32_t tau;   /* 16-bit codes */
    bool same;      /* the input's size: copied */
} hdrg_temporal_t;

/* a job's frame (the job's at 0, the others after it, nearer first) */
typedef struct {
    const uint8_t *rows;
    const float *luma, *mean;
    size_t stride;
    uint32_t weight; /* by its distance */
} hdrg_st_frame_t;

/* a job's work for the pool's rows */
typedef struct {
    hdrg_temporal_t *st;
    hdrg_st_frame_t frames[HDRG_ST_REFS + 1u];
    uint8_t *out;
    size_t out_stride;
    uint32_t count, ref; /* the other frames; the one whose blocks are refined */
} hdrg_st_work_t;

/* a plane of w x h at (x, y) of its pixels, bilinear, the edges clamped */
static inline float hdrg_st_bilinear(const float *plane, uint32_t w, uint32_t h, float x, float y)
{
    uint32_t x0, y0, x1, y1;
    float fx, fy;

    x = HDRG_ST_MIN(HDRG_ST_MAX(x, 0.0f), (float)(w - 1u));
    y = HDRG_ST_MIN(HDRG_ST_MAX(y, 0.0f), (float)(h - 1u));
    x0 = (uint32_t)x;
    y0 = (uint32_t)y;
    x1 = HDRG_ST_MIN(x0 + 1u, w - 1u);
    y1 = HDRG_ST_MIN(y0 + 1u, h - 1u);
    fx = x - (float)x0;
    fy = y - (float)y0;

    return (1.0f - fy) *
               ((1.0f - fx) * plane[(size_t)y0 * w + x0] + fx * plane[(size_t)y0 * w + x1]) +
           fy * ((1.0f - fx) * plane[(size_t)y1 * w + x0] + fx * plane[(size_t)y1 * w + x1]);
}

/* 1 within tau of a difference, 0 from 3 tau, in proportion between */
static inline float hdrg_st_match(float d, float tau)
{
    return (d <= tau) ? 1.0f : ((d >= 3.0f * tau) ? 0.0f : (3.0f * tau - d) / (2.0f * tau));
}

/* the edge-adaptive kernel of a frame at (x, y) of its pixels, the edges clamped */
static void hdrg_st_sample(const hdrg_temporal_t *st, const hdrg_st_frame_t *frame, float x,
                           float y, float out[3])
{
    const uint16_t *src;
    uint8_t i, j, c;
    int64_t ix, iy, sx, sy;
    float taps[HDRG_EDGE_TAPS][HDRG_EDGE_TAPS][3], l[HDRG_EDGE_TAPS][HDRG_EDGE_TAPS];

    ix = (int64_t)floorf(x);
    iy = (int64_t)floorf(y);

    for (i = 0; i < HDRG_EDGE_TAPS; i++) {
        sy = iy + i - 2;
        sy = (sy < 0) ? 0 : ((sy > (int64_t)st->in_h - 1) ? (int64_t)st->in_h - 1 : sy);

        for (j = 0; j < HDRG_EDGE_TAPS; j++) {
            sx = ix + j - 2;
            sx = (sx < 0) ? 0 : ((sx > (int64_t)st->in_w - 1) ? (int64_t)st->in_w - 1 : sx);
            src = (const uint16_t *)(const void *)(frame->rows + (size_t)sy * frame->stride) +
                  (size_t)sx * 3u;
            l[i][j] = frame->luma[(size_t)sy * st->in_w + (size_t)sx];

            for (c = 0; c < 3u; c++) {
                taps[i][j][c] = (float)src[c] * HDRG_ST_UNIT;
            }
        }
    }

    hdrg_edge_pixel(taps, l, x - floorf(x), y - floorf(y), &st->config, out);
}

/* the sum of |differences| of the 3 x 3 means of block (bx, by) of the job's frame and ref's at v
 */
static float hdrg_st_cost(const hdrg_temporal_t *st, const hdrg_st_frame_t *own,
                          const hdrg_st_frame_t *ref, uint32_t bx, uint32_t by, int32_t vx,
                          int32_t vy)
{
    uint32_t x, y;
    float sum, d;

    sum = 0.0f;

    for (y = by * HDRG_ST_BLOCK; y < HDRG_ST_MIN((by + 1u) * HDRG_ST_BLOCK, st->in_h); y++) {
        for (x = bx * HDRG_ST_BLOCK; x < HDRG_ST_MIN((bx + 1u) * HDRG_ST_BLOCK, st->in_w); x++) {
            d = own->mean[(size_t)y * st->in_w + x] -
                hdrg_st_bilinear(ref->mean, st->in_w, st->in_h, (float)x + (float)vx / 16.0f,
                                 (float)y + (float)vy / 16.0f);
            sum += (d < 0.0f) ? -d : d;
        }
    }

    return sum;
}

/*
 * the blocks of rows [row_begin, row_end) of the reference work->ref: the
 * vector from blockme at the block's center refined (+-1/2, 1/4, 1/8 of a
 * pixel about the best) and the block's match
 */
static void hdrg_st_block_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_st_work_t *work = (const hdrg_st_work_t *)arg;
    const hdrg_temporal_t *st = work->st;
    const hdrg_st_frame_t *own = &work->frames[0], *ref = &work->frames[1u + work->ref];
    uint32_t bx, by, cx, cy, n;
    int32_t vx, vy, bvx, bvy, step, dx, dy;
    size_t i;
    float cost, best;

    (void)worker;

    for (by = row_begin; by < row_end; by++) {
        for (bx = 0; bx < st->blocks_w; bx++) {
            cx = HDRG_ST_MIN(bx * HDRG_ST_BLOCK + HDRG_ST_BLOCK / 2u, st->in_w - 1u);
            cy = HDRG_ST_MIN(by * HDRG_ST_BLOCK + HDRG_ST_BLOCK / 2u, st->in_h - 1u);
            hdrg_bme_vector(st->bme.fields[0], st->bme.blocks_w[0], st->bme.blocks_h[0], cx, cy,
                            &bvx, &bvy);
            best = hdrg_st_cost(st, own, ref, bx, by, bvx, bvy);

            for (step = 8; step >= 2; step /= 2) {
                vx = bvx;
                vy = bvy;

                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }

                        cost = hdrg_st_cost(st, own, ref, bx, by, vx + dx * step, vy + dy * step);

                        if (cost < best) {
                            best = cost;
                            bvx = vx + dx * step;
                            bvy = vy + dy * step;
                        }
                    }
                }
            }

            i = (size_t)by * st->blocks_w + bx;
            n = (HDRG_ST_MIN((bx + 1u) * HDRG_ST_BLOCK, st->in_w) - bx * HDRG_ST_BLOCK) *
                (HDRG_ST_MIN((by + 1u) * HDRG_ST_BLOCK, st->in_h) - by * HDRG_ST_BLOCK);
            st->vectors[work->ref][2u * i] = bvx;
            st->vectors[work->ref][2u * i + 1u] = bvy;
            st->weights[work->ref][i] =
                (uint16_t)(hdrg_st_match(best / (float)n, (float)st->tau * HDRG_ST_UNIT) *
                               (float)HDRG_ST_ONE +
                           0.5f);
        }
    }
}

/* a reference's vector (1/16 pixel) and match at (x, y): bilinear between the blocks' centers */
static void hdrg_st_vector(const hdrg_temporal_t *st, uint32_t ref, float x, float y, float *vx,
                           float *vy, float *match)
{
    const uint16_t *w = st->weights[ref];
    const int32_t *v = st->vectors[ref];
    uint32_t bx0, by0, bx1, by1;
    size_t i00, i01, i10, i11;
    float gx, gy, fx, fy, w00, w01, w10, w11;

    gx = HDRG_ST_MIN(HDRG_ST_MAX((x - 3.5f) / (float)HDRG_ST_BLOCK, 0.0f),
                     (float)(st->blocks_w - 1u));
    gy = HDRG_ST_MIN(HDRG_ST_MAX((y - 3.5f) / (float)HDRG_ST_BLOCK, 0.0f),
                     (float)(st->blocks_h - 1u));
    bx0 = (uint32_t)gx;
    by0 = (uint32_t)gy;
    bx1 = HDRG_ST_MIN(bx0 + 1u, st->blocks_w - 1u);
    by1 = HDRG_ST_MIN(by0 + 1u, st->blocks_h - 1u);
    fx = gx - (float)bx0;
    fy = gy - (float)by0;
    w00 = (1.0f - fx) * (1.0f - fy);
    w01 = fx * (1.0f - fy);
    w10 = (1.0f - fx) * fy;
    w11 = fx * fy;
    i00 = (size_t)by0 * st->blocks_w + bx0;
    i01 = (size_t)by0 * st->blocks_w + bx1;
    i10 = (size_t)by1 * st->blocks_w + bx0;
    i11 = (size_t)by1 * st->blocks_w + bx1;
    *vx = w00 * (float)v[2u * i00] + w01 * (float)v[2u * i01] + w10 * (float)v[2u * i10] +
          w11 * (float)v[2u * i11];
    *vy = w00 * (float)v[2u * i00 + 1u] + w01 * (float)v[2u * i01 + 1u] +
          w10 * (float)v[2u * i10 + 1u] + w11 * (float)v[2u * i11 + 1u];
    *match =
        (w00 * (float)w[i00] + w01 * (float)w[i01] + w10 * (float)w[i10] + w11 * (float)w[i11]) /
        (float)HDRG_ST_ONE;
}

/* output rows [row_begin, row_end): the job's frame's kernel and the others' along their motion */
static void hdrg_st_out_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_st_work_t *work = (const hdrg_st_work_t *)arg;
    const hdrg_temporal_t *st = work->st;
    const float tau = (float)st->tau * HDRG_ST_UNIT;
    uint32_t x, y, k;
    uint16_t *dst;
    uint8_t c;
    float u, v, own[3], other[3], sum[3], total, vx, vy, match, w, d;

    (void)worker;

    for (y = row_begin; y < row_end; y++) {
        dst = (uint16_t *)(void *)(work->out + (size_t)y * work->out_stride);
        v = (0.5f + (float)y) * st->config.scale_y - 0.5f;

        for (x = 0; x < st->out_w; x++) {
            u = (0.5f + (float)x) * st->config.scale_x - 0.5f;
            hdrg_st_sample(st, &work->frames[0], u, v, own);
            sum[0] = own[0];
            sum[1] = own[1];
            sum[2] = own[2];
            total = 1.0f;

            for (k = 0; k < work->count; k++) {
                hdrg_st_vector(st, k, u, v, &vx, &vy, &match);
                hdrg_st_sample(st, &work->frames[1u + k], u + vx / 16.0f, v + vy / 16.0f, other);
                d = hdrg_st_bilinear(work->frames[1u + k].mean, st->in_w, st->in_h, u + vx / 16.0f,
                                     v + vy / 16.0f) -
                    hdrg_st_bilinear(work->frames[0].mean, st->in_w, st->in_h, u, v);
                w = (float)work->frames[1u + k].weight / (float)HDRG_ST_ONE * match *
                    hdrg_st_match((d < 0.0f) ? -d : d, tau);

                for (c = 0; c < 3u; c++) {
                    sum[c] += w * other[c];
                }

                total += w;
            }

            for (c = 0; c < 3u; c++) {
                dst[x * 3u + c] =
                    (uint16_t)(HDRG_ST_MIN(HDRG_ST_MAX(sum[c] / total, 0.0f), 1.0f) * 65535.0f +
                               0.5f);
            }
        }
    }
}

/* the instance's memory for frames of width x height (kept while the size stays) */
static hdrg_status_t hdrg_st_fit(hdrg_temporal_t *st, uint32_t width, uint32_t height,
                                 const hdrg_plugin_api_t *api)
{
    const size_t pixels = (size_t)width * height;
    hdrg_status_t status;
    uint64_t total;
    uint32_t k;
    uint8_t *block;
    size_t blocks, bytes;

    if (st->in_w == width && st->in_h == height && st->block != NULL) {
        return HDRG_OK;
    }

    /* a failed refit leaves nothing of the old size behind */
    st->in_w = 0;
    st->in_h = 0;
    api->mem_free(api, st->block);
    st->block = NULL;
    status = hdrg_bme_fit(&st->bme, width, height, api);

    for (k = 0; status == HDRG_OK && k < st->slots; k++) {
        status = hdrg_bme_pyramid_fit(&st->bme, &st->pyramids[k], api);
        st->pyramid_index[k] = UINT32_MAX;
        st->luma_index[k] = UINT32_MAX;
    }

    if (status != HDRG_OK) {
        return status;
    }

    st->blocks_w = (width + HDRG_ST_BLOCK - 1u) / HDRG_ST_BLOCK;
    st->blocks_h = (height + HDRG_ST_BLOCK - 1u) / HDRG_ST_BLOCK;
    blocks = (size_t)st->blocks_w * st->blocks_h;
    total = 2u * (uint64_t)st->slots * pixels * sizeof(float) +
            (uint64_t)(st->slots - 1u) * blocks * (2u * sizeof(int32_t) + sizeof(uint16_t));
    if (total > SIZE_MAX) {
        return HDRG_ERR_NOMEM;
    }

    bytes = (size_t)total;
    st->block = api->mem_alloc(api, bytes);
    if (st->block == NULL) {
        return HDRG_ERR_NOMEM;
    }

    block = (uint8_t *)st->block;

    for (k = 0; k < st->slots; k++) {
        st->lumas[k] = (float *)(void *)(block + (size_t)k * pixels * sizeof(float));
        st->means[k] = (float *)(void *)(block + (st->slots + (size_t)k) * pixels * sizeof(float));
    }

    block += 2u * (size_t)st->slots * pixels * sizeof(float);

    for (k = 0; k + 1u < st->slots; k++) {
        st->vectors[k] = (int32_t *)(void *)(block + (size_t)k * blocks * 2u * sizeof(int32_t));
    }

    block += (size_t)(st->slots - 1u) * blocks * 2u * sizeof(int32_t);

    for (k = 0; k + 1u < st->slots; k++) {
        st->weights[k] = (uint16_t *)(void *)(block + (size_t)k * blocks * sizeof(uint16_t));
    }

    return HDRG_OK;
}

/* frame index's luma and its 3 x 3 mean into the frame (made from its rows when not kept) */
static void hdrg_st_luma(hdrg_temporal_t *st, uint32_t index, hdrg_st_frame_t *frame)
{
    const uint32_t slot = index % st->slots;
    const uint16_t *row;
    uint32_t x, y, i, j, sx, sy;
    float *luma, *mean, sum;

    luma = st->lumas[slot];
    mean = st->means[slot];
    frame->luma = luma;
    frame->mean = mean;

    if (st->luma_index[slot] == index) {
        return;
    }

    for (y = 0; y < st->in_h; y++) {
        row = (const uint16_t *)(const void *)(frame->rows + (size_t)y * frame->stride);

        for (x = 0; x < st->in_w; x++) {
            luma[(size_t)y * st->in_w + x] =
                hdrg_edge_luma(row[x * 3u], row[x * 3u + 1u], row[x * 3u + 2u]);
        }
    }

    for (y = 0; y < st->in_h; y++) {
        for (x = 0; x < st->in_w; x++) {
            sum = 0.0f;

            for (i = 0; i < 3u; i++) {
                sy = HDRG_ST_MIN(HDRG_ST_MAX(y + i, 1u) - 1u, st->in_h - 1u);

                for (j = 0; j < 3u; j++) {
                    sx = HDRG_ST_MIN(HDRG_ST_MAX(x + j, 1u) - 1u, st->in_w - 1u);
                    sum += luma[(size_t)sy * st->in_w + sx];
                }
            }

            mean[(size_t)y * st->in_w + x] = sum / 9.0f;
        }
    }

    st->luma_index[slot] = index;
}

/* frame index's pyramid (made from rows when it is not kept) */
static const hdrg_bme_pyramid_t *hdrg_st_pyramid(hdrg_temporal_t *st, uint32_t index,
                                                 const uint8_t *rows, size_t stride,
                                                 const hdrg_plugin_api_t *api)
{
    const uint32_t slot = index % st->slots;

    if (st->pyramid_index[slot] != index) {
        hdrg_bme_pyramid(&st->bme, &st->pyramids[slot], rows, stride, api);
        st->pyramid_index[slot] = index;
    }

    return &st->pyramids[slot];
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_st_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_temporal_t *st;
    hdrg_status_t status;
    int64_t past, future;
    double threshold;

    status = api->option_get_int(options, HDRG_LIT("past"), &past);
    status =
        (status == HDRG_OK) ? api->option_get_int(options, HDRG_LIT("future"), &future) : status;
    status = (status == HDRG_OK) ? api->option_get_float(options, HDRG_LIT("threshold"), &threshold)
                                 : status;
    status = (status == HDRG_OK) ? api->filter_set_window(filter, (uint32_t)past, (uint32_t)future)
                                 : status;

    if (status != HDRG_OK) {
        return status;
    }

    st = (hdrg_temporal_t *)api->mem_calloc(api, 1, sizeof(*st));
    if (st == NULL) {
        return HDRG_ERR_NOMEM;
    }

    st->past = (uint32_t)past;
    st->future = (uint32_t)future;
    st->slots = st->past + st->future + 1u;
    st->tau = (uint32_t)HDRG_ST_MAX(threshold * 257.0 + 0.5, 1.0);
    st->config.stretch = (float)HDRG_EDGE_STRETCH;
    st->config.sharpen = (float)HDRG_EDGE_SHARPEN;

    *inst = st;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_st_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_temporal_t *st = (hdrg_temporal_t *)inst;
    uint32_t k;

    hdrg_bme_free(&st->bme, api);

    for (k = 0; k < HDRG_ST_SLOTS; k++) {
        hdrg_bme_pyramid_free(&st->pyramids[k], api);
    }

    api->mem_free(api, st->block);
    api->mem_free(api, st);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_st_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_temporal_t *st = (hdrg_temporal_t *)inst;
    hdrg_status_t status;
    uint32_t width, height, format, out_w, out_h;

    status = api->nego_input(nego, &width, &height, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &out_w, &out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || width == 0 || height == 0 || out_w < width ||
        out_h < height) {
        return HDRG_ERR_UNSUPPORTED;
    }

    st->same = out_w == width && out_h == height;

    /* the same size: copied (no motion) */
    if (!st->same) {
        status = hdrg_st_fit(st, width, height, api);
        if (status != HDRG_OK) {
            return status;
        }
    }

    st->in_w = width;
    st->in_h = height;
    st->out_w = out_w;
    st->out_h = out_h;
    st->config.scale_x = (float)width / (float)out_w;
    st->config.scale_y = (float)height / (float)out_h;

    return api->nego_set_output(nego, out_w, out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_st_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_bme_pyramid_t *own, *other;
    hdrg_temporal_t *st = (hdrg_temporal_t *)inst;
    hdrg_st_work_t work;
    hdrg_status_t status;
    uint32_t index, past, future, shot_past, shot_future, begin, end, y, k, side;
    int32_t offset, offsets[HDRG_ST_REFS];

    memset(&work, 0, sizeof(work));
    work.st = st;

    status = api->job_input(job, 0, &work.frames[0].rows, &work.frames[0].stride, &begin, &end);
    status = (status == HDRG_OK) ? api->job_output(job, 0, &work.out, &work.out_stride) : status;
    status = (status == HDRG_OK) ? api->job_window(job, &index, &past, &future) : status;
    status = (status == HDRG_OK) ? api->job_shot(job, &shot_past, &shot_future) : status;

    if (status != HDRG_OK) {
        return status;
    }

    if (st->same) {
        for (y = 0; y < st->out_h; y++) {
            memcpy(work.out + (size_t)y * work.out_stride,
                   work.frames[0].rows + (size_t)y * work.frames[0].stride, (size_t)st->out_w * 6u);
        }

        return HDRG_OK;
    }

    /* the frames of the window in the job's shot: nearer first, before then after */
    past = HDRG_ST_MIN(HDRG_ST_MIN(past, shot_past), st->past);
    future = HDRG_ST_MIN(HDRG_ST_MIN(future, shot_future), st->future);

    for (offset = 1; offset <= (int32_t)HDRG_ST_REACH; offset++) {
        for (side = 0; side < 2u; side++) {
            if ((side == 0u && (uint32_t)offset > past) ||
                (side == 1u && (uint32_t)offset > future)) {
                continue;
            }

            status = api->job_input_frame(job, (side == 0u) ? -offset : offset, 0,
                                          &work.frames[1u + work.count].rows,
                                          &work.frames[1u + work.count].stride, &begin, &end);
            if (status != HDRG_OK) {
                return status;
            }

            work.frames[1u + work.count].weight = hdrg_st_distance[offset];
            offsets[work.count] = (side == 0u) ? -offset : offset;
            work.count++;
        }
    }

    hdrg_st_luma(st, index, &work.frames[0]);
    own = (work.count > 0u)
              ? hdrg_st_pyramid(st, index, work.frames[0].rows, work.frames[0].stride, api)
              : NULL;

    for (k = 0; k < work.count; k++) {
        hdrg_st_luma(st, (uint32_t)((int32_t)index + offsets[k]), &work.frames[1u + k]);
        other = hdrg_st_pyramid(st, (uint32_t)((int32_t)index + offsets[k]),
                                work.frames[1u + k].rows, work.frames[1u + k].stride, api);
        hdrg_bme_search(&st->bme, own, other, 0, HDRG_ST_LAMBDA, api);
        work.ref = k;
        hdrg_bme_rows(api, st->blocks_h, 1u, hdrg_st_block_rows, &work);
    }

    hdrg_bme_rows(api, st->out_h, 4u, hdrg_st_out_rows, &work);

    return HDRG_OK;
}

extern hdrg_status_t hdrg_filter_temporal_register(hdrg_plugin_t *plugin,
                                                   const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_LIT(HDRG_ST_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_int(cls, HDRG_LIT("past"), 2, 0, (int64_t)HDRG_ST_REACH,
                                       HDRG_LIT("the frames before read"));
    status = (status == HDRG_OK)
                 ? api->class_add_option_int(cls, HDRG_LIT("future"), 2, 0, (int64_t)HDRG_ST_REACH,
                                             HDRG_LIT("the frames after read (each a frame of "
                                                      "delay)"))
                 : status;
    status = (status == HDRG_OK)
                 ? api->class_add_option_float(
                       cls, HDRG_LIT("threshold"), HDRG_ST_THRESHOLD, 0.25, 64.0,
                       HDRG_LIT("8-bit codes a frame's sample may differ from the job's frame's "
                                "fully (none from 3 times it)"))
                 : status;
    status = (status == HDRG_OK) ? api->class_set_stages(cls, HDRG_STAGE_SCALE) : status;
    status = (status == HDRG_OK) ? api->class_set_flags(cls, HDRG_FILTER_DETERMINISTIC) : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, hdrg_st_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, hdrg_st_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, hdrg_st_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_process(cls, hdrg_st_process) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}
