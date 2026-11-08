/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "mctf.h"
#include "parts.h"

#define HDRG_LIT(s)      s, sizeof(s) - 1u
#define HDRG_MT_ID       "hdrg.mctf"
#define HDRG_MT_LICENSE  "Apache-2.0 OR MIT"
#define HDRG_MT_COPY     "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_MT_REACH    HDRG_MCTF_REACH           /* the window each way at most ... */
#define HDRG_MT_REFS     (2u * HDRG_MT_REACH)      /* ... its frames besides the job's */
#define HDRG_MT_SLOTS    (2u * HDRG_MT_REACH + 1u) /* the pyramids kept, by frame index */
#define HDRG_MT_ONE      256u                      /* weights in 1/256 */
#define HDRG_MT_TAU      3.0                       /* tau: this x strength x the noise level */
#define HDRG_MT_LAMBDA   (HDRG_BME_LAMBDA / 16u) /* the search's penalty: a sixteenth of interp's */
#define HDRG_MCTF_LEVELS 256u                    /* tau by the luma's top 8 bits */
#define HDRG_MT_BLOCK_LO 1u                      /* a block's mean |difference| over the noise: */
#define HDRG_MT_BLOCK_HI 3u                      /* ... all of the frame to none */
#define HDRG_MT_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define HDRG_MT_MAX(a, b) (((a) > (b)) ? (a) : (b))

/* the frames' weights by their distance from the job's (1/256: VVenC's MCTF's 0.85, 0.57) */
static const uint32_t hdrg_mt_distance[HDRG_MT_REACH + 1u] = {256u, 218u, 146u};

/*
 * hdrg.mctf: motion-compensated temporal noise reduction of the PREFILTER
 * stage, in integers (deterministic, the same
 * whatever the threads): a window of the frames before and after the job's
 * (past / future, 2 each by default; fewer at the stream's ends). For each
 * of them the block motion estimation (hdrgrading/plugins/blockme.h, phase 0: the
 * job's frame's blocks found in it; a sixteenth of hdrg.interp.mc's
 * penalty off the predictor: denoising wants the true motion of small
 * moves) and each block's mean |difference| of the luma along its vector;
 * each output pixel the mean of the job's frame and the others read along
 * their vectors (bilinear at 1/16 pixel), each weighted by its distance, by
 * the mean difference d of its three samples from the job's
 * (exp(-(d / tau)^2 / 2), tau = 3 x strength x the Y' noise at the pixel's
 * level from HDRG_SD_NOISE) and by its block's match (all at the noise
 * level, none at 3 times it: a cut, an occlusion, a wrong vector).
 * The same weight for the three channels (no hue shift). Without noise to
 * remove (no side data, a clean frame, strength 0) it passes through.
 */
typedef struct {
    hdrg_bme_t bme;
    hdrg_bme_pyramid_t pyramids[HDRG_MT_SLOTS];
    uint32_t pyramid_index[HDRG_MT_SLOTS]; /* the frames the pyramids are of (UINT32_MAX: none) */
    hdrg_bme_vec_t *fields[HDRG_MT_REFS];  /* each other frame's finest field ... */
    uint16_t *blocks[HDRG_MT_REFS];        /* ... and its blocks' weights (1/256) */
    uint8_t *side;                         /* the output's HDRG_SD_MOTION (the fields') */
    int32_t *vector_rows;  /* each worker's rows of the fields' columns (hdrg_bme_vector_row) ... */
    uint32_t *weight_rows; /* ... and of the weights' (hdrg_mt_weight_row) */
    void *block;           /* the fields', the weights' and the rows' memory */
    uint32_t taus[HDRG_MCTF_LEVELS];   /* tau (16-bit codes, at least 1) by the luma's level */
    uint32_t block_lo, block_hi;       /* a block's mean |difference| (16-bit codes): all / none */
    uint16_t weights[HDRG_MCTF_STEPS]; /* exp(-(i / 16)^2 / 2) in 1/256 */
    uint32_t width, height;            /* configure's (0: none yet) */
    uint32_t past, future;             /* the window */
    bool active;                       /* there is noise to remove */
    double strength;
} hdrg_mctf_t;

/* a job's other frame */
typedef struct {
    const uint8_t *rows;
    size_t stride;
    uint32_t weight; /* by its distance */
} hdrg_mt_ref_t;

/* a job's work for the pool's rows */
typedef struct {
    hdrg_mctf_t *mt;
    hdrg_mt_ref_t refs[HDRG_MT_REFS];
    const uint8_t *rows;
    uint8_t *out;
    size_t stride, out_stride;
    uint32_t count, ref; /* the other frames; the one whose blocks are weighed */
} hdrg_mt_work_t;

/* the luma of pixel x of RGB16 row (blockme's weights) */
static inline uint32_t hdrg_mt_luma(const uint16_t *row, uint32_t x)
{
    return (HDRG_BME_KR * row[x * 3u] + HDRG_BME_KG * row[x * 3u + 1u] +
            HDRG_BME_KB * row[x * 3u + 2u] + 32768u) >>
           16u;
}

/*
 * the horizontal half of a block weight grid's bilinear between the block
 * centers at pixel x, from a row's columns (hdrg_mt_weight_row)
 */
static inline uint32_t hdrg_mt_weight_at(const uint32_t *columns, uint32_t bw, uint32_t x)
{
    uint32_t bx0, bx1, wx;
    int32_t px;

    px = (int32_t)x * 16 - 120;
    px = (px < 0) ? 0 : px;
    bx0 = (uint32_t)px >> 8u;
    wx = (uint32_t)px & 255u;

    if (bx0 + 1u >= bw) {
        bx0 = bw - 1u;
        wx = 0;
    }

    bx1 = (bx0 + 1u < bw) ? bx0 + 1u : bx0;

    return ((256u - wx) * columns[bx0] + wx * columns[bx1] + 32768u) >> 16u;
}

/* the vertical half for row y of a block weight grid (bw x bh) into columns (bw, x 256) */
static void hdrg_mt_weight_row(const uint16_t *blocks, uint32_t bw, uint32_t bh, uint32_t y,
                               uint32_t *columns)
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
        columns[b] = (256u - wy) * blocks[(size_t)by0 * bw + b] + wy * blocks[(size_t)by1 * bw + b];
    }
}

/* block rows of work->ref's weights: its match along the vectors against the noise */
static void hdrg_mt_block_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_mt_work_t *work = (const hdrg_mt_work_t *)arg;
    const hdrg_mctf_t *mt = work->mt;
    const hdrg_bme_t *bme = &mt->bme;
    const uint32_t bw = bme->blocks_w[0], w = bme->sizes[0][0], h = bme->sizes[0][1];
    hdrg_bme_vec_t v;
    uint32_t bx, by, n, mean;
    uint16_t *blocks;

    (void)worker;

    blocks = mt->blocks[work->ref];

    for (by = row_begin; by < row_end; by++) {
        for (bx = 0; bx < bw; bx++) {
            v = bme->fields[0][(size_t)by * bw + bx];
            n = HDRG_MT_MIN(HDRG_BME_BLOCK, w - bx * HDRG_BME_BLOCK) *
                HDRG_MT_MIN(HDRG_BME_BLOCK, h - by * HDRG_BME_BLOCK);
            mean = (uint32_t)(hdrg_bme_cost(bme, 0, bx, by, v, v) / ((uint64_t)n * 256u));
            blocks[(size_t)by * bw + bx] =
                (uint16_t)((mean <= mt->block_lo)   ? HDRG_MT_ONE
                           : (mean >= mt->block_hi) ? 0u
                                                    : (HDRG_MT_ONE * (mt->block_hi - mean) +
                                                       (mt->block_hi - mt->block_lo) / 2u) /
                                                          (mt->block_hi - mt->block_lo));
            hdrg_mctf_motion_block(bw, bme->blocks_h[0], work->ref, (size_t)by * bw + bx, v.x, v.y,
                                   mean, blocks[(size_t)by * bw + bx], mt->side);
        }
    }
}

/* rows of the output: the job's frame and the others along their vectors, weighted */
static void hdrg_mt_out_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_mt_work_t *work = (const hdrg_mt_work_t *)arg;
    const hdrg_mctf_t *mt = work->mt;
    const uint32_t bw = mt->bme.blocks_w[0], bh = mt->bme.blocks_h[0];
    const uint16_t *src;
    uint64_t num[3], den;
    uint32_t y, x, k, s[3], d, tau, i, w, *weights;
    uint16_t *dst;
    uint8_t c;
    int32_t vx, vy, *vectors;

    vectors = mt->vector_rows + (size_t)worker * HDRG_MT_REFS * 2u * bw;
    weights = mt->weight_rows + (size_t)worker * HDRG_MT_REFS * bw;

    for (y = row_begin; y < row_end; y++) {
        src = (const uint16_t *)(const void *)(work->rows + (size_t)y * work->stride);
        dst = (uint16_t *)(void *)(work->out + (size_t)y * work->out_stride);

        for (k = 0; k < work->count; k++) {
            hdrg_bme_vector_row(mt->fields[k], bw, bh, y, vectors + (size_t)k * 2u * bw);
            hdrg_mt_weight_row(mt->blocks[k], bw, bh, y, weights + (size_t)k * bw);
        }

        for (x = 0; x < mt->width; x++) {
            tau = mt->taus[hdrg_mt_luma(src, x) >> 8u];
            den = (uint64_t)HDRG_MT_ONE << 8u;

            for (c = 0; c < 3u; c++) {
                num[c] = (uint64_t)src[x * 3u + c] << 16u;
            }

            for (k = 0; k < work->count; k++) {
                hdrg_bme_vector_at(vectors + (size_t)k * 2u * bw, bw, x, &vx, &vy);
                hdrg_bme_rgb(work->refs[k].rows, work->refs[k].stride, mt->width, mt->height,
                             (int32_t)x * 16 + vx, (int32_t)y * 16 + vy, s);
                d = 0;

                for (c = 0; c < 3u; c++) {
                    d += (s[c] > ((uint32_t)src[x * 3u + c] << 8u))
                             ? s[c] - ((uint32_t)src[x * 3u + c] << 8u)
                             : ((uint32_t)src[x * 3u + c] << 8u) - s[c];
                }

                d /= 3u;

                i = HDRG_MT_MIN((uint32_t)(((uint64_t)(d >> 8u) << 4u) / tau),
                                HDRG_MCTF_STEPS - 1u);
                w = (work->refs[k].weight * mt->weights[i] *
                         hdrg_mt_weight_at(weights + (size_t)k * bw, bw, x) +
                     32768u) >>
                    16u;

                for (c = 0; c < 3u; c++) {
                    num[c] += (uint64_t)w * s[c];
                }

                den += (uint64_t)w << 8u;
            }

            for (c = 0; c < 3u; c++) {
                dst[x * 3u + c] = (uint16_t)HDRG_MT_MIN((num[c] + den / 2u) / den, 65535u);
            }
        }
    }
}

/* the fields, the weights and the pyramids for width x height (kept while the size stays) */
static hdrg_status_t hdrg_mt_fit(hdrg_mctf_t *mt, uint32_t width, uint32_t height,
                                 const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;
    uint32_t k;
    uint8_t *block;
    size_t blocks, bytes, rows, at;

    if (mt->width == width && mt->height == height) {
        return HDRG_OK;
    }

    mt->width = 0;
    mt->height = 0;
    api->mem_free(api, mt->block);
    api->mem_free(api, mt->side);
    mt->block = NULL;
    mt->side = NULL;
    status = hdrg_bme_fit(&mt->bme, width, height, api);

    for (k = 0; status == HDRG_OK && k < HDRG_MT_SLOTS; k++) {
        mt->pyramid_index[k] = UINT32_MAX;
        status = hdrg_bme_pyramid_fit(&mt->bme, &mt->pyramids[k], api);
    }

    if (status != HDRG_OK) {
        return status;
    }

    blocks = (size_t)mt->bme.blocks_w[0] * mt->bme.blocks_h[0];
    rows = (size_t)api->run_workers(api) * HDRG_MT_REFS * mt->bme.blocks_w[0];
    bytes = HDRG_MT_REFS * blocks * (sizeof(hdrg_bme_vec_t) + sizeof(uint16_t)) +
            rows * (2u * sizeof(int32_t) + sizeof(uint32_t)) + 2u * sizeof(int32_t);
    block = (uint8_t *)api->mem_alloc(api, bytes);
    mt->side = (uint8_t *)api->mem_alloc(
        api, hdrg_bme_motion_size(mt->bme.blocks_w[0], mt->bme.blocks_h[0], HDRG_MT_REFS));

    if (block == NULL || mt->side == NULL) {
        api->mem_free(api, block);

        return HDRG_ERR_NOMEM;
    }

    for (k = 0; k < HDRG_MT_REFS; k++) {
        mt->fields[k] = (hdrg_bme_vec_t *)(void *)(block + k * blocks * sizeof(hdrg_bme_vec_t));
        mt->blocks[k] =
            (uint16_t *)(void *)(block + HDRG_MT_REFS * blocks * sizeof(hdrg_bme_vec_t) +
                                 k * blocks * sizeof(uint16_t));
    }

    /* the rows' scratch after them, on a word */
    at = HDRG_MT_REFS * blocks * (sizeof(hdrg_bme_vec_t) + sizeof(uint16_t));
    at = (at + sizeof(int32_t) - 1u) / sizeof(int32_t) * sizeof(int32_t);
    mt->vector_rows = (int32_t *)(void *)(block + at);
    mt->weight_rows = (uint32_t *)(void *)(block + at + rows * 2u * sizeof(int32_t));
    mt->block = block;
    mt->width = width;
    mt->height = height;

    return HDRG_OK;
}

/* frame index's pyramid (made from rows when it is not kept) */
static const hdrg_bme_pyramid_t *hdrg_mt_pyramid(hdrg_mctf_t *mt, uint32_t index,
                                                 const uint8_t *rows, size_t stride,
                                                 const hdrg_plugin_api_t *api)
{
    const uint32_t slot = index % HDRG_MT_SLOTS;

    if (mt->pyramid_index[slot] != index) {
        hdrg_bme_pyramid(&mt->bme, &mt->pyramids[slot], rows, stride, api);
        mt->pyramid_index[slot] = index;
    }

    return &mt->pyramids[slot];
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mt_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_mctf_t *mt;
    hdrg_status_t status;
    uint32_t i;
    int64_t past, future;
    double strength;

    status = api->option_get_int(options, HDRG_LIT("past"), &past);

    if (status == HDRG_OK) {
        status = api->option_get_int(options, HDRG_LIT("future"), &future);
    }

    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_LIT("strength"), &strength);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_window(filter, (uint32_t)past, (uint32_t)future);
    }

    if (status != HDRG_OK) {
        return status;
    }

    mt = (hdrg_mctf_t *)api->mem_calloc(api, 1, sizeof(*mt));
    if (mt == NULL) {
        return HDRG_ERR_NOMEM;
    }

    mt->past = (uint32_t)past;
    mt->future = (uint32_t)future;
    mt->strength = strength;

    for (i = 0; i < HDRG_MCTF_STEPS; i++) {
        mt->weights[i] = (uint16_t)hdrg_mctf_weight(i);
    }

    *inst = mt;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_mt_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_mctf_t *mt = (hdrg_mctf_t *)inst;
    uint32_t k;

    hdrg_bme_free(&mt->bme, api);

    for (k = 0; k < HDRG_MT_SLOTS; k++) {
        hdrg_bme_pyramid_free(&mt->pyramids[k], api);
    }

    api->mem_free(api, mt->block);
    api->mem_free(api, mt->side);
    api->mem_free(api, mt);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mt_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_mctf_t *mt = (hdrg_mctf_t *)inst;
    hdrg_status_t status;
    uint32_t width, height, format;

    status = api->nego_input(nego, &width, &height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || width == 0 || height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_mctf_noise(nego, api, mt->strength, mt->taus, &mt->block_lo, &mt->block_hi,
                             &mt->active);
    mt->active = mt->active && mt->past + mt->future > 0u;

    if (status == HDRG_OK && mt->active) {
        status = hdrg_mt_fit(mt, width, height, api);
    }

    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, width, height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !mt->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mt_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_bme_pyramid_t *own, *other;
    hdrg_mctf_t *mt = (hdrg_mctf_t *)inst;
    hdrg_mt_work_t work;
    hdrg_status_t status;
    uint32_t index, past, future, begin, end, y, k;
    int32_t offset, offsets[HDRG_MT_REFS];

    memset(&work, 0, sizeof(work));
    work.mt = mt;

    status = api->job_input(job, 0, &work.rows, &work.stride, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &work.out, &work.out_stride);
    }

    if (status == HDRG_OK) {
        status = api->job_window(job, &index, &past, &future);
    }

    if (status != HDRG_OK) {
        return status;
    }

    past = HDRG_MT_MIN(past, mt->past);
    future = HDRG_MT_MIN(future, mt->future);

    /* the frames of the window: nearer first, before then after */
    for (offset = 1; offset <= (int32_t)HDRG_MT_REACH; offset++) {
        for (k = 0; k < 2u; k++) {
            if ((k == 0u && (uint32_t)offset > past) || (k == 1u && (uint32_t)offset > future)) {
                continue;
            }

            status = api->job_input_frame(job, (k == 0u) ? -offset : offset, 0,
                                          &work.refs[work.count].rows,
                                          &work.refs[work.count].stride, &begin, &end);
            if (status != HDRG_OK) {
                return status;
            }

            work.refs[work.count].weight = hdrg_mt_distance[offset];
            offsets[work.count] = (k == 0u) ? -offset : offset;
            work.count++;
        }
    }

    /* none: the frame itself */
    if (work.count == 0u) {
        for (y = 0; y < mt->height; y++) {
            memcpy(work.out + (size_t)y * work.out_stride, work.rows + (size_t)y * work.stride,
                   (size_t)mt->width * 6u);
        }

        return HDRG_OK;
    }

    own = hdrg_mt_pyramid(mt, index, work.rows, work.stride, api);
    hdrg_bme_motion_head(mt->bme.blocks_w[0], mt->bme.blocks_h[0], work.count, offsets, 0u,
                         mt->side);
    k = 0;

    for (offset = 1; offset <= (int32_t)HDRG_MT_REACH; offset++) {
        for (y = 0; y < 2u; y++) {
            if ((y == 0u && (uint32_t)offset > past) || (y == 1u && (uint32_t)offset > future)) {
                continue;
            }

            other =
                hdrg_mt_pyramid(mt, (y == 0u) ? index - (uint32_t)offset : index + (uint32_t)offset,
                                work.refs[k].rows, work.refs[k].stride, api);
            hdrg_bme_search(&mt->bme, own, other, 0, HDRG_MT_LAMBDA, api);
            memcpy(mt->fields[k], mt->bme.fields[0],
                   (size_t)mt->bme.blocks_w[0] * mt->bme.blocks_h[0] * sizeof(hdrg_bme_vec_t));
            work.ref = k;
            hdrg_bme_rows(api, mt->bme.blocks_h[0], 1u, hdrg_mt_block_rows, &work);
            k++;
        }
    }

    hdrg_bme_rows(api, mt->height, 8u, hdrg_mt_out_rows, &work);

    /* the motion to the window's frames (a host without the side data takes the frame alone) */
    status = api->job_set_side_data(
        job, HDRG_SD_MOTION, HDRG_SD_MOTION_VERSION, mt->side,
        hdrg_bme_motion_size(mt->bme.blocks_w[0], mt->bme.blocks_h[0], work.count));

    return (status == HDRG_ERR_UNSUPPORTED) ? HDRG_OK : status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_mctf_init(hdrg_plugin_t *plugin,
                                                            const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_MT_ID),
                                  HDRG_LIT("Motion-compensated temporal denoise"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_MT_LICENSE), HDRG_LIT(HDRG_MT_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_MT_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = hdrg_mctf_options(cls, api);
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_mt_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_mt_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_mt_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_mt_process);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_mctf_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_mctf_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_mctf_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}

extern void hdrg_mctf_motion_block(uint32_t blocks_w, uint32_t blocks_h, uint32_t k, size_t i,
                                   int32_t vx, int32_t vy, uint32_t mean, uint32_t weight,
                                   uint8_t *side)
{
    hdrg_sd_motion_block_t record;

    record.mv_x = (int16_t)HDRG_MT_MIN(HDRG_MT_MAX(2 * vx, INT16_MIN), INT16_MAX);
    record.mv_y = (int16_t)HDRG_MT_MIN(HDRG_MT_MAX(2 * vy, INT16_MIN), INT16_MAX);
    record.cost = (uint16_t)HDRG_MT_MIN(mean, 0xfffeu);
    record.confidence = (uint8_t)((HDRG_MT_MIN(weight, HDRG_MT_ONE) * 255u + 128u) / HDRG_MT_ONE);
    record.occlusion = 0;
    hdrg_bme_motion_block(blocks_w, blocks_h, k, i, &record, side);
}

extern bool hdrg_mctf_table(const hdrg_bme_t *bme, uint32_t words[HDRG_MCTF_WORDS],
                            uint64_t *pyramid, uint64_t *motion)
{
    const uint64_t blocks = (uint64_t)bme->blocks_w[0] * bme->blocks_h[0];
    uint32_t level, k, i, *row;

    memset(words, 0, HDRG_MCTF_WORDS * sizeof(uint32_t));
    *pyramid = 0;
    *motion = 0;

    for (level = 0; level < bme->levels; level++) {
        row = words + level * HDRG_MCTF_TABLE;
        row[0] = bme->sizes[level][0];
        row[1] = bme->sizes[level][1];
        row[4] = bme->blocks_w[level];
        row[5] = bme->blocks_h[level];
    }

    for (k = 0; k < HDRG_MCTF_SLOTS; k++) {
        for (level = 0; level < bme->levels; level++) {
            words[HDRG_MCTF_PLANES + k * HDRG_BME_LEVELS + level] = (uint32_t)*pyramid;
            *pyramid += (uint64_t)bme->sizes[level][0] * bme->sizes[level][1];
        }
    }

    /* the side data's first: each other frame's blocks' words, then their finest fields */
    for (k = 0; k < HDRG_MCTF_REFS; k++) {
        words[HDRG_MCTF_WEIGHTS + k] = (uint32_t)*motion;
        *motion += blocks;
    }

    for (k = 0; k < HDRG_MCTF_REFS; k++) {
        words[HDRG_MCTF_FIELDS + k * HDRG_BME_LEVELS] = (uint32_t)*motion;
        *motion += 2u * blocks;
    }

    for (k = 0; k < HDRG_MCTF_REFS; k++) {
        for (level = 1; level < bme->levels; level++) {
            words[HDRG_MCTF_FIELDS + k * HDRG_BME_LEVELS + level] = (uint32_t)*motion;
            *motion += 2u * (uint64_t)bme->blocks_w[level] * bme->blocks_h[level];
        }
    }

    words[HDRG_MCTF_SCRATCH] = (uint32_t)*motion;
    *motion += 2u * blocks;

    for (i = 0; i < HDRG_MCTF_STEPS; i++) {
        words[HDRG_MCTF_STEPS_AT + i] = hdrg_mctf_weight(i);
    }

    return *pyramid <= UINT32_MAX && *motion <= UINT32_MAX && *pyramid * 4u <= (uint64_t)SIZE_MAX &&
           *motion * 4u <= (uint64_t)SIZE_MAX;
}

extern size_t hdrg_mctf_motion_prefix(const hdrg_bme_t *bme)
{
    return 3u * HDRG_MCTF_REFS * (size_t)bme->blocks_w[0] * bme->blocks_h[0] * sizeof(uint32_t);
}

extern void hdrg_mctf_motion_read(const hdrg_bme_t *bme, const uint8_t *prefix, uint32_t count,
                                  const int32_t *offsets, uint8_t *side)
{
    const uint32_t bw = bme->blocks_w[0], bh = bme->blocks_h[0];
    const size_t blocks = (size_t)bw * bh;
    uint32_t k, word;
    int32_t v[2];
    size_t i;

    hdrg_bme_motion_head(bw, bh, count, offsets, 0u, side);

    for (k = 0; k < count; k++) {
        for (i = 0; i < blocks; i++) {
            memcpy(&word, prefix + (k * blocks + i) * sizeof(uint32_t), sizeof(word));
            memcpy(v, prefix + (HDRG_MCTF_REFS * blocks + 2u * (k * blocks + i)) * sizeof(int32_t),
                   sizeof(v));
            hdrg_mctf_motion_block(bw, bh, k, i, v[0], v[1], word >> 16, word & 0xffffu, side);
        }
    }
}

extern uint32_t hdrg_mctf_weight(uint32_t i)
{
    return (uint32_t)((float)HDRG_MT_ONE *
                          hdrg_math_expf(-0.5f * ((float)i / 16.0f) * ((float)i / 16.0f)) +
                      0.5f);
}

extern hdrg_status_t hdrg_mctf_noise(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                     double strength, uint32_t taus[HDRG_MCTF_LEVELS],
                                     uint32_t *block_lo, uint32_t *block_hi, bool *active)
{
    hdrg_status_t status;
    uint32_t i;
    double level[HDRG_DENOISE_BINS], chroma[2], sigma, tau;

    status = hdrg_denoise_settings(nego, api, strength, &sigma, level, chroma, active);
    *active = *active && sigma > 0.0;

    /* tau at each level of the luma's top 8 bits, the blocks' bounds at the frame's noise */
    for (i = 0; *active && i < HDRG_MCTF_LEVELS; i++) {
        tau = HDRG_MT_TAU * strength *
              hdrg_denoise_level(level, sigma, ((double)i + 0.5) / (double)HDRG_MCTF_LEVELS);
        taus[i] = (uint32_t)HDRG_MT_MAX(tau + 0.5, 1.0);
    }

    *block_lo = (uint32_t)(HDRG_MT_BLOCK_LO * sigma + 0.5);
    *block_hi = HDRG_MT_MAX((uint32_t)(HDRG_MT_BLOCK_HI * sigma + 0.5), *block_lo + 1u);

    return status;
}

extern hdrg_status_t hdrg_mctf_options(hdrg_class_t *cls, const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->class_add_option_float(
        cls, HDRG_LIT("strength"), 1.0, 0.0, 4.0,
        HDRG_LIT("the samples' threshold over the measured noise level (0 = off)"));
    if (status == HDRG_OK) {
        status =
            api->class_add_option_int(cls, HDRG_LIT("past"), (int64_t)HDRG_MCTF_REACH, 0,
                                      (int64_t)HDRG_MCTF_REACH, HDRG_LIT("the frames before read"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(
            cls, HDRG_LIT("future"), (int64_t)HDRG_MCTF_REACH, 0, (int64_t)HDRG_MCTF_REACH,
            HDRG_LIT("the frames after read (each a frame of delay)"));
    }

    return status;
}
