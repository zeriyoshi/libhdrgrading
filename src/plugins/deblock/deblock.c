/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_DB_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HDRG_DB_MAX(a, b) ((a) > (b) ? (a) : (b))
#define HDRG_DB_ID        "hdrg.deblock"
#define HDRG_DB_LICENSE   "Apache-2.0 OR MIT"
#define HDRG_DB_COPY      "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_DB_GRID      8u /* the blocks' grid, from the frame's origin */
#define HDRG_DB_REACH     4u /* the samples a boundary reads on each side */
#define HDRG_DB_TOP       65535.0
#define HDRG_DB_RATIO     1.1f   /* the blockiness above which the grid's steps count as blocks */
#define HDRG_DB_KR        13933u /* BT.709 luma weights in 1/65536 for the decisions */
#define HDRG_DB_KG        46871u
#define HDRG_DB_KB        4732u
#define HDRG_DB_TC        1.5 /* thresholds over the block step and the noise */
#define HDRG_DB_ALPHA     6.0
#define HDRG_DB_ALPHA_SD  2.0
#define HDRG_DB_BETA      2.0
#define HDRG_DB_BETA_SD   2.5

/*
 * Deblocking of the PREFILTER stage: the boundaries of the
 * 8-pixel grid, vertical ones first, then horizontal ones on that result,
 * in the manner of H.264's loop filter. A line across a boundary is
 * filtered when its Y' step there is below alpha and its steps next to the
 * boundary below beta; with both sides flat (and a small step) the strong
 * filter spreads the step over three samples a side, else the normal one
 * moves the two samples next to it (and the next ones on flat sides) by at
 * most tc. The decisions read Y', the filters run on R, G and B alike.
 * There is no quantizer to read: alpha, beta and tc follow the frame's
 * HDRG_SD_NOISE side data, the mean step the grid adds (times the strength)
 * and the noise level; without it, or with a blockiness at most
 * HDRG_DB_RATIO, the input passes unchanged (reported as passed through). Integer arithmetic on the
 * 16-bit codes; a boundary reads only samples no other one moves, so any band split or worker count
 * gives the same output.
 */
typedef struct {
    uint32_t width, height;
    int32_t alpha, beta, tc;
    bool active;
    double strength;
} hdrg_deblock_t;

/* Y' of the sample at s */
static inline int32_t hdrg_db_luma(const uint16_t *s)
{
    return (int32_t)(((uint32_t)s[0] * HDRG_DB_KR + (uint32_t)s[1] * HDRG_DB_KG +
                      (uint32_t)s[2] * HDRG_DB_KB + 32768u) >>
                     16);
}

static inline int32_t hdrg_db_abs(int32_t v)
{
    return (v < 0) ? -v : v;
}

static inline int32_t hdrg_db_clip(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* floor(v / 2^s) */
static inline int32_t hdrg_db_shift(int32_t v, uint8_t s)
{
    return (v >= 0) ? (int32_t)((uint32_t)v >> s)
                    : -(int32_t)(((uint32_t)-v + (1u << s) - 1u) >> s);
}

/*
 * the line across the boundary whose first sample after it (q0) is at q,
 * samples step uint16_t apart: p3 p2 p1 p0 | q0 q1 q2 q3
 */
static void hdrg_db_line(const hdrg_deblock_t *db, uint16_t *q, size_t step)
{
    const int32_t tc = db->tc;
    uint16_t *base = q - HDRG_DB_REACH * step;
    uint8_t c, i;
    int32_t y[8], v[8], out[6], d, ap, aq, delta, mid;
    bool strong;

    for (i = 0; i < 8u; i++) {
        y[i] = hdrg_db_luma(base + (size_t)i * step);
    }

    d = y[4] - y[3];

    if (hdrg_db_abs(d) >= db->alpha || hdrg_db_abs(y[2] - y[3]) >= db->beta ||
        hdrg_db_abs(y[5] - y[4]) >= db->beta) {
        return;
    }

    ap = hdrg_db_abs(y[1] - y[3]);
    aq = hdrg_db_abs(y[6] - y[4]);
    strong = ap < db->beta && aq < db->beta && hdrg_db_abs(d) < db->alpha / 4 + tc;

    for (c = 0; c < 3u; c++) {
        for (i = 0; i < 8u; i++) {
            v[i] = (int32_t)base[(size_t)i * step + c];
        }

        if (strong) {
            out[0] = (2 * v[0] + 3 * v[1] + v[2] + v[3] + v[4] + 4) >> 3;
            out[1] = (v[1] + v[2] + v[3] + v[4] + 2) >> 2;
            out[2] = (v[1] + 2 * v[2] + 2 * v[3] + 2 * v[4] + v[5] + 4) >> 3;
            out[3] = (v[2] + 2 * v[3] + 2 * v[4] + 2 * v[5] + v[6] + 4) >> 3;
            out[4] = (v[3] + v[4] + v[5] + v[6] + 2) >> 2;
            out[5] = (v[3] + v[4] + v[5] + 3 * v[6] + 2 * v[7] + 4) >> 3;

            for (i = 0; i < 6u; i++) {
                base[(size_t)(i + 1u) * step + c] = (uint16_t)hdrg_db_clip(
                    hdrg_db_clip(out[i], v[i + 1u] - 2 * tc, v[i + 1u] + 2 * tc), 0, 65535);
            }

            continue;
        }

        delta = hdrg_db_clip(hdrg_db_shift((v[4] - v[3]) * 4 + (v[2] - v[5]) + 4, 3), -tc, tc);
        mid = (v[3] + v[4] + 1) >> 1;
        out[1] = v[2];
        out[4] = v[5];

        if (ap < db->beta) {
            out[1] += hdrg_db_clip(hdrg_db_shift(v[1] + mid - 2 * v[2], 1), -tc / 2, tc / 2);
        }

        if (aq < db->beta) {
            out[4] += hdrg_db_clip(hdrg_db_shift(v[6] + mid - 2 * v[5], 1), -tc / 2, tc / 2);
        }

        base[2u * step + c] = (uint16_t)hdrg_db_clip(out[1], 0, 65535);
        base[3u * step + c] = (uint16_t)hdrg_db_clip(v[3] + delta, 0, 65535);
        base[4u * step + c] = (uint16_t)hdrg_db_clip(v[4] - delta, 0, 65535);
        base[5u * step + c] = (uint16_t)hdrg_db_clip(out[4], 0, 65535);
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_db_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_deblock_t *db;
    hdrg_status_t status;

    (void)filter;

    db = (hdrg_deblock_t *)api->mem_calloc(api, 1, sizeof(hdrg_deblock_t));
    if (db == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &db->strength);
    if (status != HDRG_OK) {
        api->mem_free(api, db);

        return status;
    }

    *inst = db;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_db_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_db_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_deblock_t *db = (hdrg_deblock_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &db->width, &db->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || db->width == 0 || db->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_deblock_thresholds(nego, api, db->strength, &db->alpha, &db->beta, &db->tc,
                                     &db->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, db->width, db->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !db->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_db_input_rows(void *inst, uint32_t out_begin,
                                                         uint32_t out_end, uint32_t *in_begin,
                                                         uint32_t *in_end,
                                                         const hdrg_plugin_api_t *api)
{
    const hdrg_deblock_t *db = (const hdrg_deblock_t *)inst;
    const uint32_t reach = HDRG_DB_GRID;

    (void)api;

    if (out_begin >= out_end || out_end > db->height) {
        return HDRG_ERR_INVALID_ARG;
    }

    *in_begin = (out_begin > reach) ? out_begin - reach : 0u;
    *in_end = HDRG_DB_MIN(out_end + reach, db->height);

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_db_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_deblock_t *db = (const hdrg_deblock_t *)inst;
    const uint8_t *in;
    const size_t row = (size_t)db->width * 3u;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, x;
    uint16_t *work;
    uint8_t *out;
    size_t in_stride, out_stride;
    void *scratch;

    status = api->job_rows(job, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_input(job, 0, &in, &in_stride, &in_begin, &in_end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &out, &out_stride);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (begin < in_begin || end > in_end) {
        return HDRG_ERR_INTERNAL;
    }

    status = api->job_scratch(job, (size_t)(in_end - in_begin) * row * sizeof(uint16_t), &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    work = (uint16_t *)scratch;

    for (y = in_begin; y < in_end; y++) {
        memcpy(work + (size_t)(y - in_begin) * row, in + (size_t)(y - in_begin) * in_stride,
               row * sizeof(uint16_t));
    }

    /* the vertical boundaries of every input row */
    for (y = in_begin; y < in_end; y++) {
        for (x = HDRG_DB_GRID; x + HDRG_DB_REACH <= db->width; x += HDRG_DB_GRID) {
            hdrg_db_line(db, work + (size_t)(y - in_begin) * row + (size_t)x * 3u, 3u);
        }
    }

    /* the horizontal boundaries that move output rows, all their rows read */
    for (y = HDRG_DB_GRID; y + HDRG_DB_REACH <= db->height; y += HDRG_DB_GRID) {
        if (y + HDRG_DB_REACH - 1u <= begin || y >= end + HDRG_DB_REACH - 1u ||
            y < in_begin + HDRG_DB_REACH || y + HDRG_DB_REACH > in_end) {
            continue;
        }

        for (x = 0; x < db->width; x++) {
            hdrg_db_line(db, work + (size_t)(y - in_begin) * row + (size_t)x * 3u, row);
        }
    }

    for (y = begin; y < end; y++) {
        memcpy(out + (size_t)(y - begin) * out_stride, work + (size_t)(y - in_begin) * row,
               row * sizeof(uint16_t));
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_deblock_thresholds(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                             double strength, int32_t *alpha, int32_t *beta,
                                             int32_t *tc, bool *active)
{
    hdrg_status_t status;
    uint32_t version;
    size_t needed;
    double step, sigma;
    float noise[HDRG_SD_NOISE_FLOATS];

    *alpha = 0;
    *beta = 0;
    *tc = 0;
    *active = false;
    status = api->nego_side_data(nego, HDRG_SD_NOISE, &version, noise, sizeof(noise), &needed);

    if (status == HDRG_OK && version == HDRG_SD_NOISE_VERSION && needed == sizeof(noise) &&
        noise[0] >= 0.0f) {
        step = HDRG_DB_MAX((double)noise[12], 0.0) * HDRG_DB_TOP * strength;
        sigma = (double)noise[0] * HDRG_DB_TOP;
        *tc = (int32_t)(HDRG_DB_TC * step + 0.5);
        *alpha = (int32_t)(HDRG_DB_ALPHA * step + HDRG_DB_ALPHA_SD * sigma + 0.5);
        *beta = (int32_t)(HDRG_DB_BETA * step + HDRG_DB_BETA_SD * sigma + 0.5);
        *active = noise[11] > HDRG_DB_RATIO && *tc > 0;
    } else if (status != HDRG_OK && status != HDRG_ERR_NOT_FOUND) {
        return status;
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_deblock_init(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_DB_ID), HDRG_LIT("Deblocking"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_DB_LICENSE), HDRG_LIT(HDRG_DB_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_DB_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status =
        api->class_add_option_float(cls, HDRG_LIT("strength"), 1.0, 0.0, 4.0,
                                    HDRG_LIT("thresholds over the measured block step (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_db_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_db_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_db_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_db_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_db_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_deblock_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_deblock_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_deblock_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}
