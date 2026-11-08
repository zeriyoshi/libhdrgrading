/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/plugins/blockme.h"
#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_MC_ID        "hdrg.interp.mc"
#define HDRG_MC_ONE       65536u
#define HDRG_MC_HALF      32768u
#define HDRG_MC_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define HDRG_MC_MIN(a, b) (((a) < (b)) ? (a) : (b))

/* two samples this apart (x 256): the mix starts toward the nearer frame, alone from HI */
#define HDRG_MC_NEAR_LO (8u * 257u * 256u)
#define HDRG_MC_NEAR_HI (32u * 257u * 256u)

/* a block's match this far apart on average (16-bit codes): sure (255) down to none (0) */
#define HDRG_MC_SURE   (8u * 257u)
#define HDRG_MC_UNSURE (32u * 257u)

/*
 * hdrg.interp.mc: motion-compensated interpolation of two RGB16 frames at a
 * phase, in integers (deterministic, and the same whatever the threads):
 * - the vectors of the block motion estimation (hdrgrading/plugins/blockme.h) at
 *   the output's time;
 * - each output pixel from both frames along its vector (the finest
 *   level's, bilinear between the block centers), mixed by the phase and,
 *   where the two samples disagree (an occlusion, a wrong vector), toward
 *   the nearer frame's.
 * The output's HDRG_SD_MOTION: the finest field (blocks of 16 x 16 pixels),
 * each block's mean |difference| of the luma along its vector and the
 * confidence from it (HDRG_MC_SURE to HDRG_MC_UNSURE).
 * Its rows run on the context's threads (run_parallel).
 */
typedef struct {
    hdrg_bme_t bme;
    hdrg_bme_pyramid_t a, b;
    uint32_t a_index, b_index; /* the frames the pyramids are of (UINT32_MAX: none): kept for
                                  the next job, whose first frame is this one's second */
    uint8_t *side;             /* the output's HDRG_SD_MOTION (side_size bytes) */
    size_t side_size;
    uint32_t width, height; /* configure's (0: none yet) */
} hdrg_mc_t;

/* a job's work for the pool's rows */
typedef struct {
    hdrg_mc_t *mc;
    const uint8_t *a, *b;
    uint8_t *out;
    size_t a_stride, b_stride, out_stride;
    uint32_t phase;
} hdrg_mc_work_t;

/* block rows of the finest field into the side data's blocks: the vector, the match, the confidence
 */
static void hdrg_mc_record_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_mc_work_t *work = (const hdrg_mc_work_t *)arg;
    const hdrg_bme_t *bme = &work->mc->bme;
    const uint32_t bw = bme->blocks_w[0], w = bme->sizes[0][0], h = bme->sizes[0][1];
    hdrg_sd_motion_block_t record;
    hdrg_bme_vec_t v;
    uint64_t sad;
    uint32_t bx, by, n, mean;
    uint8_t *blocks;

    (void)worker;

    blocks = work->mc->side + sizeof(hdrg_sd_motion_t) + sizeof(hdrg_sd_motion_field_t);

    for (by = row_begin; by < row_end; by++) {
        for (bx = 0; bx < bw; bx++) {
            v = bme->fields[0][(size_t)by * bw + bx];
            sad = hdrg_bme_cost(bme, 0, bx, by, v, v);
            n = HDRG_MC_MIN(HDRG_BME_BLOCK, w - bx * HDRG_BME_BLOCK) *
                HDRG_MC_MIN(HDRG_BME_BLOCK, h - by * HDRG_BME_BLOCK);
            mean = (uint32_t)(sad / ((uint64_t)n * 256u));
            record.mv_x = (int16_t)HDRG_MC_MIN(HDRG_MC_MAX(2 * v.x, INT16_MIN), INT16_MAX);
            record.mv_y = (int16_t)HDRG_MC_MIN(HDRG_MC_MAX(2 * v.y, INT16_MIN), INT16_MAX);
            record.cost = (uint16_t)HDRG_MC_MIN(mean, 0xfffeu);
            record.confidence = (mean <= HDRG_MC_SURE) ? UINT8_MAX
                                : (mean >= HDRG_MC_UNSURE)
                                    ? 0u
                                    : (uint8_t)((255u * (HDRG_MC_UNSURE - mean) +
                                                 (HDRG_MC_UNSURE - HDRG_MC_SURE) / 2u) /
                                                (HDRG_MC_UNSURE - HDRG_MC_SURE));
            record.occlusion = 0;
            memcpy(blocks + ((size_t)by * bw + bx) * sizeof(record), &record, sizeof(record));
        }
    }
}

/* rows of the output: both frames along each pixel's vector, mixed */
static void hdrg_mc_out_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_mc_work_t *work = (const hdrg_mc_work_t *)arg;
    const hdrg_mc_t *mc = work->mc;
    const uint32_t near = (work->phase >= HDRG_MC_HALF) ? HDRG_MC_ONE : 0u;
    uint64_t mix;
    uint32_t y, x, sa[3], sb[3], d, t, wb;
    uint16_t *dst;
    uint8_t c;
    int32_t vx, vy, ax, ay;

    (void)worker;

    for (y = row_begin; y < row_end; y++) {
        dst = (uint16_t *)(void *)(work->out + (size_t)y * work->out_stride);

        for (x = 0; x < mc->width; x++) {
            hdrg_bme_vector(mc->bme.fields[0], mc->bme.blocks_w[0], mc->bme.blocks_h[0], x, y, &vx,
                            &vy);
            ax = hdrg_bme_round((int64_t)work->phase * vx, 16u);
            ay = hdrg_bme_round((int64_t)work->phase * vy, 16u);
            hdrg_bme_rgb(work->a, work->a_stride, mc->width, mc->height, (int32_t)x * 16 - ax,
                         (int32_t)y * 16 - ay, sa);
            hdrg_bme_rgb(work->b, work->b_stride, mc->width, mc->height,
                         (int32_t)x * 16 + (vx - ax), (int32_t)y * 16 + (vy - ay), sb);
            d = 0;

            for (c = 0; c < 3u; c++) {
                d = HDRG_MC_MAX(d, (sa[c] > sb[c]) ? sa[c] - sb[c] : sb[c] - sa[c]);
            }

            /* where the samples disagree, toward the nearer frame */
            t = (d <= HDRG_MC_NEAR_LO)   ? 0u
                : (d >= HDRG_MC_NEAR_HI) ? HDRG_MC_ONE
                                         : (uint32_t)(((uint64_t)(d - HDRG_MC_NEAR_LO) << 16u) /
                                                      (HDRG_MC_NEAR_HI - HDRG_MC_NEAR_LO));
            wb = (uint32_t)(((uint64_t)work->phase * (HDRG_MC_ONE - t) + (uint64_t)near * t +
                             HDRG_MC_HALF) >>
                            16u);

            for (c = 0; c < 3u; c++) {
                mix = (uint64_t)sa[c] * (HDRG_MC_ONE - wb) + (uint64_t)sb[c] * wb +
                      (UINT64_C(1) << 23u);
                dst[x * 3u + c] = (uint16_t)HDRG_MC_MIN(mix >> 24u, UINT64_C(65535));
            }
        }
    }
}

/* the levels, the pyramids and the side data for width x height (kept while the size stays) */
static hdrg_status_t hdrg_mc_fit(hdrg_mc_t *mc, uint32_t width, uint32_t height,
                                 const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    if (mc->width == width && mc->height == height) {
        return HDRG_OK;
    }

    mc->width = 0;
    mc->height = 0;
    mc->a_index = UINT32_MAX;
    mc->b_index = UINT32_MAX;
    api->mem_free(api, mc->side);
    mc->side = NULL;
    status = hdrg_bme_fit(&mc->bme, width, height, api);

    if (status == HDRG_OK) {
        status = hdrg_bme_pyramid_fit(&mc->bme, &mc->a, api);
    }

    if (status == HDRG_OK) {
        status = hdrg_bme_pyramid_fit(&mc->bme, &mc->b, api);
    }

    if (status != HDRG_OK) {
        return status;
    }

    mc->side_size = hdrg_bme_motion_size(mc->bme.blocks_w[0], mc->bme.blocks_h[0], 1u);
    mc->side = (uint8_t *)api->mem_alloc(api, mc->side_size);
    if (mc->side == NULL) {
        return HDRG_ERR_NOMEM;
    }

    mc->width = width;
    mc->height = height;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mc_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_mc_t *mc;

    (void)filter;
    (void)options;

    mc = (hdrg_mc_t *)api->mem_calloc(api, 1, sizeof(*mc));
    if (mc == NULL) {
        return HDRG_ERR_NOMEM;
    }

    mc->a_index = UINT32_MAX;
    mc->b_index = UINT32_MAX;
    *inst = mc;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_mc_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_mc_t *mc = (hdrg_mc_t *)inst;

    hdrg_bme_free(&mc->bme, api);
    hdrg_bme_pyramid_free(&mc->a, api);
    hdrg_bme_pyramid_free(&mc->b, api);
    api->mem_free(api, mc->side);
    api->mem_free(api, mc);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mc_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_mc_t *mc = (hdrg_mc_t *)inst;
    hdrg_status_t status;
    uint32_t width, height, format;

    status = api->nego_input(nego, &width, &height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || width == 0 || height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_mc_fit(mc, width, height, api);
    if (status != HDRG_OK) {
        return status;
    }

    return api->nego_set_output(nego, width, height, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_mc_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    hdrg_mc_t *mc = (hdrg_mc_t *)inst;
    hdrg_bme_pyramid_t swap;
    hdrg_mc_work_t work;
    hdrg_status_t status;
    uint32_t begin, end, index, past, future;

    memset(&work, 0, sizeof(work));
    work.mc = mc;

    status = api->job_input(job, 0, &work.a, &work.a_stride, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_input_frame(job, 1, 0, &work.b, &work.b_stride, &begin, &end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &work.out, &work.out_stride);
    }

    if (status == HDRG_OK) {
        status = api->job_phase(job, &work.phase);
    }

    if (status == HDRG_OK) {
        status = api->job_window(job, &index, &past, &future);
    }

    if (status != HDRG_OK) {
        return status;
    }

    /* the frames' pyramids kept: the previous job's second frame is this one's first */
    if (mc->a_index != index && mc->b_index == index) {
        swap = mc->a;
        mc->a = mc->b;
        mc->b = swap;
        mc->a_index = index;
        mc->b_index = UINT32_MAX;
    }

    if (mc->a_index != index) {
        hdrg_bme_pyramid(&mc->bme, &mc->a, work.a, work.a_stride, api);
        mc->a_index = index;
    }

    if (mc->b_index != index + 1u) {
        hdrg_bme_pyramid(&mc->bme, &mc->b, work.b, work.b_stride, api);
        mc->b_index = index + 1u;
    }

    hdrg_bme_search(&mc->bme, &mc->a, &mc->b, work.phase, HDRG_BME_LAMBDA, api);
    hdrg_bme_rows(api, mc->height, 8u, hdrg_mc_out_rows, &work);
    hdrg_bme_rows(api, mc->bme.blocks_h[0], 1u, hdrg_mc_record_rows, &work);

    hdrg_interp_motion_head(mc->bme.blocks_w[0], mc->bme.blocks_h[0], work.phase, mc->side);

    /* a host without the output's side data (none asked for) takes the frame alone */
    status = api->job_set_side_data(job, HDRG_SD_MOTION, HDRG_SD_MOTION_VERSION, mc->side,
                                    mc->side_size);

    return (status == HDRG_ERR_UNSUPPORTED) ? HDRG_OK : status;
}

extern hdrg_status_t hdrg_interp_mc_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_LIT(HDRG_MC_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_INTERP);

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_mc_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_mc_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_mc_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_mc_process);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}

extern void hdrg_interp_motion_head(uint32_t blocks_w, uint32_t blocks_h, uint32_t phase,
                                    uint8_t *side)
{
    const int32_t next = 1;

    hdrg_bme_motion_head(blocks_w, blocks_h, 1u, &next, phase, side);
}

extern void hdrg_interp_motion_records(const uint8_t *records, uint32_t blocks_w, uint32_t blocks_h,
                                       uint8_t *side)
{
    const size_t count = (size_t)blocks_w * blocks_h;
    hdrg_sd_motion_block_t record;
    uint32_t words[2];
    size_t i;

    for (i = 0; i < count; i++) {
        memcpy(words, records + i * sizeof(words), sizeof(words));
        record.mv_x = (int16_t)(uint16_t)(words[0] & 0xffffu);
        record.mv_y = (int16_t)(uint16_t)(words[0] >> 16);
        record.cost = (uint16_t)(words[1] & 0xffffu);
        record.confidence = (uint8_t)(words[1] >> 16);
        record.occlusion = 0;
        hdrg_bme_motion_block(blocks_w, blocks_h, 0u, i, &record, side);
    }
}
