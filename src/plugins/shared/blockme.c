/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/foundation/thread.h"
#include "hdrgrading/plugins/blockme.h"

#define HDRG_BME_HALF      32768u
#define HDRG_BME_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define HDRG_BME_MIN(a, b) (((a) < (b)) ? (a) : (b))

/* hdrg_bme_rows' chunks of rows */
typedef struct {
    hdrg_pool_rows_fn_t fn;
    void *arg;
    uint32_t rows, grain;
} hdrg_bme_rows_t;

/* a rows job's work: the pyramid's frame and level, or the search */
typedef struct {
    hdrg_bme_t *bme;
    hdrg_bme_pyramid_t *pyramid;
    const uint8_t *rows;
    size_t stride;
    uint32_t level;
} hdrg_bme_work_t;

static inline uint32_t hdrg_bme_abs(int32_t v)
{
    return (v < 0) ? (uint32_t)-v : (uint32_t)v;
}

/* the plane at (x, y) in 1/16 pixel, bilinear, the edges repeated: x 256 */
static inline uint32_t hdrg_bme_sample(const hdrg_bme_plane_t *plane, int32_t x, int32_t y)
{
    const uint16_t *r0, *r1;
    const int32_t top_x = (int32_t)(plane->width - 1u) * 16,
                  top_y = (int32_t)(plane->height - 1u) * 16;
    uint32_t x0, y0, x1, y1, fx, fy;

    x = (x < 0) ? 0 : ((x > top_x) ? top_x : x);
    y = (y < 0) ? 0 : ((y > top_y) ? top_y : y);
    x0 = (uint32_t)x >> 4u;
    y0 = (uint32_t)y >> 4u;
    fx = (uint32_t)x & 15u;
    fy = (uint32_t)y & 15u;
    x1 = (x0 + 1u < plane->width) ? x0 + 1u : x0;
    y1 = (y0 + 1u < plane->height) ? y0 + 1u : y0;
    r0 = plane->data + (size_t)y0 * plane->width;
    r1 = plane->data + (size_t)y1 * plane->width;

    return (16u - fx) * (16u - fy) * r0[x0] + fx * (16u - fy) * r0[x1] + (16u - fx) * fy * r1[x0] +
           fx * fy * r1[x1];
}

/* floor(v / 16) for either sign */
static inline int32_t hdrg_bme_floor16(int32_t v)
{
    return (v >= 0) ? v / 16 : -((-v + 15) / 16);
}

/*
 * whether the samples of pixels [xs, xe) x [ys, ye) moved by (ix, iy) whole
 * pixels (and a fraction) all lie inside the plane with their right and lower
 * neighbors: no edge repeated, the bilinear's four samples there
 */
static inline bool hdrg_bme_inside(const hdrg_bme_plane_t *plane, uint32_t xs, uint32_t xe,
                                   uint32_t ys, uint32_t ye, int32_t ix, int32_t iy)
{
    return (int32_t)xs + ix >= 0 && (int32_t)ys + iy >= 0 &&
           (int64_t)xe + ix < (int64_t)plane->width && (int64_t)ye + iy < (int64_t)plane->height;
}

/* the bilinear weights of fractions (fx, fy) in 1/16: x 256 in all */
static inline void hdrg_bme_weights(uint32_t fx, uint32_t fy, uint32_t w[4])
{
    w[0] = (16u - fx) * (16u - fy);
    w[1] = fx * (16u - fy);
    w[2] = (16u - fx) * fy;
    w[3] = fx * fy;
}

/* the luma of pixel x of RGB16 row */
static inline uint32_t hdrg_bme_luma(const uint16_t *row, uint32_t x)
{
    return (HDRG_BME_KR * row[x * 3u] + HDRG_BME_KG * row[x * 3u + 1u] +
            HDRG_BME_KB * row[x * 3u + 2u] + HDRG_BME_HALF) >>
           16u;
}

/* a chunk of hdrg_bme_rows' rows (run_parallel's work) */
static void HDRG_PLUGIN_CALL hdrg_bme_rows_task(void *user, uint32_t index, uint32_t worker)
{
    const hdrg_bme_rows_t *job = (const hdrg_bme_rows_t *)user;
    const uint32_t begin = index * job->grain,
                   end = (begin + job->grain < job->rows) ? begin + job->grain : job->rows;

    job->fn(begin, end, worker, job->arg);
}

/* rows of a frame's half-resolution luma (2 x 2 means of the pixels' luma) */
static void hdrg_bme_luma_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_bme_work_t *work = (const hdrg_bme_work_t *)arg;
    const hdrg_bme_t *bme = work->bme;
    const hdrg_bme_plane_t *plane = &work->pyramid->levels[0];
    const uint16_t *r0, *r1;
    uint32_t y, x, y0, y1, x0, x1;
    uint16_t *dst;

    (void)worker;

    for (y = row_begin; y < row_end; y++) {
        y0 = 2u * y;
        y1 = (y0 + 1u < bme->height) ? y0 + 1u : y0;
        r0 = (const uint16_t *)(const void *)(work->rows + (size_t)y0 * work->stride);
        r1 = (const uint16_t *)(const void *)(work->rows + (size_t)y1 * work->stride);
        dst = plane->data + (size_t)y * plane->width;

        for (x = 0; x < plane->width; x++) {
            x0 = 2u * x;
            x1 = (x0 + 1u < bme->width) ? x0 + 1u : x0;
            dst[x] = (uint16_t)((hdrg_bme_luma(r0, x0) + hdrg_bme_luma(r0, x1) +
                                 hdrg_bme_luma(r1, x0) + hdrg_bme_luma(r1, x1) + 2u) >>
                                2u);
        }
    }
}

/* rows of level work->level from the one below (2 x 2 means, the edges repeated) */
static void hdrg_bme_down_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_bme_work_t *work = (const hdrg_bme_work_t *)arg;
    const hdrg_bme_plane_t *fine = &work->pyramid->levels[work->level - 1u],
                           *coarse = &work->pyramid->levels[work->level];
    const uint16_t *r0, *r1;
    uint32_t y, x, y0, y1, x0, x1;
    uint16_t *dst;

    (void)worker;

    for (y = row_begin; y < row_end; y++) {
        y0 = 2u * y;
        y1 = (y0 + 1u < fine->height) ? y0 + 1u : y0;
        r0 = fine->data + (size_t)y0 * fine->width;
        r1 = fine->data + (size_t)y1 * fine->width;
        dst = coarse->data + (size_t)y * coarse->width;

        for (x = 0; x < coarse->width; x++) {
            x0 = 2u * x;
            x1 = (x0 + 1u < fine->width) ? x0 + 1u : x0;
            dst[x] = (uint16_t)(((uint32_t)r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2u) >> 2u);
        }
    }
}

/* the best of v + d for d of the steps (+-step and 0 on each axis), from best (its cost) */
static void hdrg_bme_refine(const hdrg_bme_t *bme, uint32_t level, uint32_t bx, uint32_t by,
                            int32_t step, hdrg_bme_vec_t pred, hdrg_bme_vec_t *best,
                            uint64_t *best_cost)
{
    hdrg_bme_vec_t center, v;
    uint64_t cost;
    int32_t dx, dy;

    center = *best;

    for (dy = -step; dy <= step; dy += step) {
        for (dx = -step; dx <= step; dx += step) {
            if (dx == 0 && dy == 0) {
                continue;
            }

            v.x = center.x + dx;
            v.y = center.y + dy;
            cost = hdrg_bme_cost(bme, level, bx, by, v, pred);

            if (cost < *best_cost) {
                *best = v;
                *best_cost = cost;
            }
        }
    }
}

/* block rows of a level's field: the coarsest searched, the finer ones refined from the parent */
static void hdrg_bme_search_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_bme_work_t *work = (const hdrg_bme_work_t *)arg;
    const hdrg_bme_t *bme = work->bme;
    const hdrg_bme_vec_t *parent;
    const uint32_t level = work->level, bw = bme->blocks_w[level];
    const bool coarsest = level + 1u == bme->levels;
    hdrg_bme_vec_t best, v, pred, zero, preds[HDRG_BME_PREDICTORS];
    uint64_t cost, best_cost;
    uint32_t bx, by, px, py, pw, ph, k;
    int32_t dx, dy;

    (void)worker;

    zero.x = 0;
    zero.y = 0;
    parent = coarsest ? NULL : bme->fields[level + 1u];
    pw = coarsest ? 0u : bme->blocks_w[level + 1u];
    ph = coarsest ? 0u : bme->blocks_h[level + 1u];

    for (by = row_begin; by < row_end; by++) {
        for (bx = 0; bx < bw; bx++) {
            if (coarsest) {
                best = zero;
                best_cost = hdrg_bme_cost(bme, level, bx, by, zero, zero);

                for (dy = -HDRG_BME_RANGE; dy <= HDRG_BME_RANGE; dy++) {
                    for (dx = -HDRG_BME_RANGE; dx <= HDRG_BME_RANGE; dx++) {
                        v.x = dx * HDRG_BME_QUARTER;
                        v.y = dy * HDRG_BME_QUARTER;
                        cost = hdrg_bme_cost(bme, level, bx, by, v, zero);

                        if (cost < best_cost) {
                            best = v;
                            best_cost = cost;
                        }
                    }
                }

                /* the finest level searched alone (a small frame): refined to 1/2 and 1/4 pixel */
                if (level == 0u) {
                    hdrg_bme_refine(bme, level, bx, by, HDRG_BME_QUARTER / 2, zero, &best,
                                    &best_cost);
                    hdrg_bme_refine(bme, level, bx, by, 1, zero, &best, &best_cost);
                }

                bme->fields[level][(size_t)by * bw + bx] = best;

                continue;
            }

            /* the parent block's vector and its neighbors', at this level's scale */
            px = (bx / 2u < pw) ? bx / 2u : pw - 1u;
            py = (by / 2u < ph) ? by / 2u : ph - 1u;
            preds[0] = parent[(size_t)py * pw + px];
            preds[1] = parent[(size_t)py * pw + ((px > 0) ? px - 1u : px)];
            preds[2] = parent[(size_t)py * pw + ((px + 1u < pw) ? px + 1u : px)];
            preds[3] = parent[(size_t)((py > 0) ? py - 1u : py) * pw + px];
            preds[4] = parent[(size_t)((py + 1u < ph) ? py + 1u : py) * pw + px];

            for (k = 0; k < HDRG_BME_PREDICTORS; k++) {
                preds[k].x *= 2;
                preds[k].y *= 2;
            }

            pred = preds[0];
            best = pred;
            best_cost = hdrg_bme_cost(bme, level, bx, by, pred, pred);

            for (k = 1; k < HDRG_BME_PREDICTORS; k++) {
                cost = hdrg_bme_cost(bme, level, bx, by, preds[k], pred);

                if (cost < best_cost) {
                    best = preds[k];
                    best_cost = cost;
                }
            }

            hdrg_bme_refine(bme, level, bx, by, HDRG_BME_QUARTER, pred, &best, &best_cost);
            hdrg_bme_refine(bme, level, bx, by, HDRG_BME_QUARTER / 2, pred, &best, &best_cost);

            if (level == 0u) {
                hdrg_bme_refine(bme, level, bx, by, 1, pred, &best, &best_cost);
            }

            bme->fields[level][(size_t)by * bw + bx] = best;
        }
    }
}

/* the middle of 9 values (an insertion sort) */
static int32_t hdrg_bme_middle(int32_t *v)
{
    uint32_t i, j;
    int32_t t;

    for (i = 1; i < 9u; i++) {
        t = v[i];

        for (j = i; j > 0 && v[j - 1u] > t; j--) {
            v[j] = v[j - 1u];
        }

        v[j] = t;
    }

    return v[4];
}

/* block rows of the 3 x 3 median of a level's field (each axis) into the scratch */
static void hdrg_bme_median_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_bme_work_t *work = (const hdrg_bme_work_t *)arg;
    const hdrg_bme_t *bme = work->bme;
    const hdrg_bme_vec_t *field = bme->fields[work->level];
    const uint32_t bw = bme->blocks_w[work->level], bh = bme->blocks_h[work->level];
    uint32_t bx, by, n, x, y;
    int32_t xs[9], ys[9], dx, dy;

    (void)worker;

    for (by = row_begin; by < row_end; by++) {
        for (bx = 0; bx < bw; bx++) {
            n = 0;

            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    x = (uint32_t)HDRG_BME_MIN(HDRG_BME_MAX((int32_t)bx + dx, 0), (int32_t)bw - 1);
                    y = (uint32_t)HDRG_BME_MIN(HDRG_BME_MAX((int32_t)by + dy, 0), (int32_t)bh - 1);
                    xs[n] = field[(size_t)y * bw + x].x;
                    ys[n] = field[(size_t)y * bw + x].y;
                    n++;
                }
            }

            bme->median[(size_t)by * bw + bx].x = hdrg_bme_middle(xs);
            bme->median[(size_t)by * bw + bx].y = hdrg_bme_middle(ys);
        }
    }
}

extern void hdrg_bme_rows(const hdrg_plugin_api_t *api, uint32_t rows, uint32_t grain,
                          hdrg_pool_rows_fn_t fn, void *arg)
{
    hdrg_bme_rows_t job;

    job.fn = fn;
    job.arg = arg;
    job.rows = rows;
    job.grain = grain;

    (void)api->run_parallel(api, (rows + grain - 1u) / grain, hdrg_bme_rows_task, &job);
}

extern void hdrg_bme_shape(hdrg_bme_t *bme, uint32_t width, uint32_t height)
{
    uint32_t level, w, h;

    w = (width + 1u) / 2u;
    h = (height + 1u) / 2u;
    level = 0;

    /* the levels down to HDRG_BME_COARSE across (at least one) */
    for (;;) {
        bme->sizes[level][0] = w;
        bme->sizes[level][1] = h;
        bme->blocks_w[level] = (w + HDRG_BME_BLOCK - 1u) / HDRG_BME_BLOCK;
        bme->blocks_h[level] = (h + HDRG_BME_BLOCK - 1u) / HDRG_BME_BLOCK;
        level++;

        if (w <= HDRG_BME_COARSE || level == HDRG_BME_LEVELS || h < 2u * HDRG_BME_BLOCK) {
            break;
        }

        w = (w + 1u) / 2u;
        h = (h + 1u) / 2u;
    }

    bme->levels = level;
}

extern hdrg_status_t hdrg_bme_fit(hdrg_bme_t *bme, uint32_t width, uint32_t height,
                                  const hdrg_plugin_api_t *api)
{
    uint32_t level;
    uint8_t *block;
    size_t bytes, at;

    if (bme->block != NULL && bme->width == width && bme->height == height) {
        return HDRG_OK;
    }

    hdrg_bme_free(bme, api);
    hdrg_bme_shape(bme, width, height);
    bytes = (size_t)bme->blocks_w[0] * bme->blocks_h[0] * sizeof(hdrg_bme_vec_t);

    for (level = 0; level < bme->levels; level++) {
        bytes += (size_t)bme->blocks_w[level] * bme->blocks_h[level] * sizeof(hdrg_bme_vec_t);
    }

    block = (uint8_t *)api->mem_alloc(api, bytes);
    if (block == NULL) {
        bme->levels = 0;
        bme->width = 0;
        bme->height = 0;

        return HDRG_ERR_NOMEM;
    }

    at = 0;

    for (level = 0; level < bme->levels; level++) {
        bme->fields[level] = (hdrg_bme_vec_t *)(void *)(block + at);
        at += (size_t)bme->blocks_w[level] * bme->blocks_h[level] * sizeof(hdrg_bme_vec_t);
    }

    bme->median = (hdrg_bme_vec_t *)(void *)(block + at);
    bme->block = block;
    bme->width = width;
    bme->height = height;

    return HDRG_OK;
}

extern void hdrg_bme_free(hdrg_bme_t *bme, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, bme->block);
    bme->block = NULL;
    bme->levels = 0;
}

extern hdrg_status_t hdrg_bme_pyramid_fit(const hdrg_bme_t *bme, hdrg_bme_pyramid_t *pyramid,
                                          const hdrg_plugin_api_t *api)
{
    uint32_t level;
    uint8_t *block;
    size_t bytes, at;

    hdrg_bme_pyramid_free(pyramid, api);
    bytes = 0;

    for (level = 0; level < bme->levels; level++) {
        bytes += (size_t)bme->sizes[level][0] * bme->sizes[level][1] * sizeof(uint16_t);
    }

    block = (uint8_t *)api->mem_alloc(api, bytes);
    if (block == NULL) {
        return HDRG_ERR_NOMEM;
    }

    at = 0;

    for (level = 0; level < bme->levels; level++) {
        pyramid->levels[level].data = (uint16_t *)(void *)(block + at);
        pyramid->levels[level].width = bme->sizes[level][0];
        pyramid->levels[level].height = bme->sizes[level][1];
        at += (size_t)bme->sizes[level][0] * bme->sizes[level][1] * sizeof(uint16_t);
    }

    pyramid->block = block;

    return HDRG_OK;
}

extern void hdrg_bme_pyramid_free(hdrg_bme_pyramid_t *pyramid, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, pyramid->block);
    memset(pyramid, 0, sizeof(*pyramid));
}

extern void hdrg_bme_pyramid(const hdrg_bme_t *bme, hdrg_bme_pyramid_t *pyramid,
                             const uint8_t *rows, size_t stride, const hdrg_plugin_api_t *api)
{
    hdrg_bme_work_t work;
    uint32_t level;

    memset(&work, 0, sizeof(work));
    work.bme = (hdrg_bme_t *)(uintptr_t)bme;
    work.pyramid = pyramid;
    work.rows = rows;
    work.stride = stride;
    hdrg_bme_rows(api, bme->sizes[0][1], 8u, hdrg_bme_luma_rows, &work);

    for (level = 1; level < bme->levels; level++) {
        work.level = level;
        hdrg_bme_rows(api, bme->sizes[level][1], 8u, hdrg_bme_down_rows, &work);
    }
}

extern void hdrg_bme_search(hdrg_bme_t *bme, const hdrg_bme_pyramid_t *a,
                            const hdrg_bme_pyramid_t *b, uint32_t phase, uint64_t lambda,
                            const hdrg_plugin_api_t *api)
{
    hdrg_bme_work_t work;
    uint32_t level;

    memset(&work, 0, sizeof(work));
    work.bme = bme;
    bme->a = a;
    bme->b = b;
    bme->phase = phase;
    bme->lambda = lambda;

    /* coarse to fine, each level's field smoothed by its median */
    for (level = bme->levels; level-- > 0;) {
        work.level = level;
        hdrg_bme_rows(api, bme->blocks_h[level], 1u, hdrg_bme_search_rows, &work);
        hdrg_bme_rows(api, bme->blocks_h[level], 1u, hdrg_bme_median_rows, &work);
        memcpy(bme->fields[level], bme->median,
               (size_t)bme->blocks_w[level] * bme->blocks_h[level] * sizeof(hdrg_bme_vec_t));
    }
}

extern uint64_t hdrg_bme_cost(const hdrg_bme_t *bme, uint32_t level, uint32_t bx, uint32_t by,
                              hdrg_bme_vec_t v, hdrg_bme_vec_t pred)
{
    const hdrg_bme_plane_t *a = &bme->a->levels[level], *b = &bme->b->levels[level];
    const uint32_t xs = bx * HDRG_BME_BLOCK, ys = by * HDRG_BME_BLOCK,
                   xe = HDRG_BME_MIN(xs + HDRG_BME_BLOCK, a->width),
                   ye = HDRG_BME_MIN(ys + HDRG_BME_BLOCK, a->height);
    const uint16_t *a0, *a1, *b0, *b1;
    const int32_t vx = v.x * HDRG_BME_QUARTER, vy = v.y * HDRG_BME_QUARTER,
                  ax = hdrg_bme_round((int64_t)bme->phase * vx, 16u),
                  ay = hdrg_bme_round((int64_t)bme->phase * vy, 16u), bx16 = vx - ax,
                  by16 = vy - ay;
    uint64_t sad;
    uint32_t i, j, x, y, sa, sb, wa[4], wb[4];
    int32_t iax, iay, ibx, iby;

    sad = 0;
    iax = hdrg_bme_floor16(-ax);
    iay = hdrg_bme_floor16(-ay);
    ibx = hdrg_bme_floor16(bx16);
    iby = hdrg_bme_floor16(by16);

    /* both blocks inside their planes: the same fractions for every sample, no edges to repeat */
    if (hdrg_bme_inside(a, xs, xe, ys, ye, iax, iay) &&
        hdrg_bme_inside(b, xs, xe, ys, ye, ibx, iby)) {
        hdrg_bme_weights((uint32_t)(-ax - iax * 16), (uint32_t)(-ay - iay * 16), wa);
        hdrg_bme_weights((uint32_t)(bx16 - ibx * 16), (uint32_t)(by16 - iby * 16), wb);

        for (y = ys; y < ye; y++) {
            a0 = a->data + (size_t)((int32_t)y + iay) * a->width + iax;
            a1 = a0 + a->width;
            b0 = b->data + (size_t)((int32_t)y + iby) * b->width + ibx;
            b1 = b0 + b->width;

            for (x = xs; x < xe; x++) {
                sa = wa[0] * a0[x] + wa[1] * a0[x + 1u] + wa[2] * a1[x] + wa[3] * a1[x + 1u];
                sb = wb[0] * b0[x] + wb[1] * b0[x + 1u] + wb[2] * b1[x] + wb[3] * b1[x + 1u];
                sad += (sa > sb) ? sa - sb : sb - sa;
            }
        }
    } else {
        for (j = 0; j < HDRG_BME_BLOCK; j++) {
            y = ys + j;

            if (y >= a->height) {
                break;
            }

            for (i = 0; i < HDRG_BME_BLOCK; i++) {
                x = xs + i;

                if (x >= a->width) {
                    break;
                }

                sa = hdrg_bme_sample(a, (int32_t)x * 16 - ax, (int32_t)y * 16 - ay);
                sb = hdrg_bme_sample(b, (int32_t)x * 16 + bx16, (int32_t)y * 16 + by16);
                sad += (sa > sb) ? sa - sb : sb - sa;
            }
        }
    }

    return sad + bme->lambda * (uint64_t)(hdrg_bme_abs(v.x - pred.x) + hdrg_bme_abs(v.y - pred.y));
}

extern void hdrg_bme_vector(const hdrg_bme_vec_t *field, uint32_t bw, uint32_t bh, uint32_t x,
                            uint32_t y, int32_t *vx, int32_t *vy)
{
    const hdrg_bme_vec_t *v00, *v10, *v01, *v11;
    uint32_t bx0, by0, bx1, by1, wx, wy;
    int64_t sx, sy;
    int32_t px, py;

    /* the block centers: (16 b + 7.5) pixels, in 1/16 of a block (256 a block) */
    px = (int32_t)x * 16 - 120;
    py = (int32_t)y * 16 - 120;
    px = (px < 0) ? 0 : px;
    py = (py < 0) ? 0 : py;
    bx0 = (uint32_t)px >> 8u;
    by0 = (uint32_t)py >> 8u;
    wx = (uint32_t)px & 255u;
    wy = (uint32_t)py & 255u;

    if (bx0 + 1u >= bw) {
        bx0 = bw - 1u;
        wx = 0;
    }

    if (by0 + 1u >= bh) {
        by0 = bh - 1u;
        wy = 0;
    }

    bx1 = (bx0 + 1u < bw) ? bx0 + 1u : bx0;
    by1 = (by0 + 1u < bh) ? by0 + 1u : by0;
    v00 = &field[(size_t)by0 * bw + bx0];
    v10 = &field[(size_t)by0 * bw + bx1];
    v01 = &field[(size_t)by1 * bw + bx0];
    v11 = &field[(size_t)by1 * bw + bx1];

    sx = (int64_t)(256u - wx) * (256u - wy) * v00->x + (int64_t)wx * (256u - wy) * v10->x +
         (int64_t)(256u - wx) * wy * v01->x + (int64_t)wx * wy * v11->x;
    sy = (int64_t)(256u - wx) * (256u - wy) * v00->y + (int64_t)wx * (256u - wy) * v10->y +
         (int64_t)(256u - wx) * wy * v01->y + (int64_t)wx * wy * v11->y;

    /* the finest level's quarter pixel is half a pixel here: x 8 in 1/16 */
    *vx = hdrg_bme_round(sx * 8, 16u);
    *vy = hdrg_bme_round(sy * 8, 16u);
}

extern void hdrg_bme_vector_row(const hdrg_bme_vec_t *field, uint32_t bw, uint32_t bh, uint32_t y,
                                int32_t *columns)
{
    uint32_t by0, by1, wy, b;
    int32_t py;

    py = (int32_t)y * 16 - 120;
    py = (py < 0) ? 0 : py;
    by0 = (uint32_t)py >> 8u;
    wy = (uint32_t)py & 255u;

    if (by0 + 1u >= bh) {
        by0 = bh - 1u;
        wy = 0;
    }

    by1 = (by0 + 1u < bh) ? by0 + 1u : by0;

    for (b = 0; b < bw; b++) {
        columns[2u * b] = (int32_t)(256u - wy) * field[(size_t)by0 * bw + b].x +
                          (int32_t)wy * field[(size_t)by1 * bw + b].x;
        columns[2u * b + 1u] = (int32_t)(256u - wy) * field[(size_t)by0 * bw + b].y +
                               (int32_t)wy * field[(size_t)by1 * bw + b].y;
    }
}

extern size_t hdrg_bme_motion_size(uint32_t blocks_w, uint32_t blocks_h, uint32_t count)
{
    return sizeof(hdrg_sd_motion_t) +
           (size_t)count * (sizeof(hdrg_sd_motion_field_t) +
                            (size_t)blocks_w * blocks_h * sizeof(hdrg_sd_motion_block_t));
}

extern void hdrg_bme_motion_head(uint32_t blocks_w, uint32_t blocks_h, uint32_t count,
                                 const int32_t *offsets, uint32_t phase, uint8_t *side)
{
    const size_t each = sizeof(hdrg_sd_motion_field_t) +
                        (size_t)blocks_w * blocks_h * sizeof(hdrg_sd_motion_block_t);
    hdrg_sd_motion_t head;
    hdrg_sd_motion_field_t field;
    uint32_t k;

    head.fields = count;
    head.reserved = 0;
    memcpy(side, &head, sizeof(head));
    memset(&field, 0, sizeof(field));
    field.source = HDRG_MOTION_INTERNAL;
    field.block_w = 2u * HDRG_BME_BLOCK;
    field.block_h = 2u * HDRG_BME_BLOCK;
    field.grid_w = blocks_w;
    field.grid_h = blocks_h;
    field.phase = phase;
    field.signal = HDRG_MOTION_SIGNAL_LUMA;
    field.shift = 1;

    for (k = 0; k < count; k++) {
        field.ref_offset = offsets[k];
        memcpy(side + sizeof(head) + k * each, &field, sizeof(field));
    }
}

extern void hdrg_bme_motion_block(uint32_t blocks_w, uint32_t blocks_h, uint32_t k, size_t i,
                                  const hdrg_sd_motion_block_t *record, uint8_t *side)
{
    const size_t each = sizeof(hdrg_sd_motion_field_t) +
                        (size_t)blocks_w * blocks_h * sizeof(hdrg_sd_motion_block_t);

    memcpy(side + sizeof(hdrg_sd_motion_t) + k * each + sizeof(hdrg_sd_motion_field_t) +
               i * sizeof(*record),
           record, sizeof(*record));
}
