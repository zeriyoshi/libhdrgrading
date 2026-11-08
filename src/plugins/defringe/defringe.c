/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_DF_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HDRG_DF_MAX(a, b) ((a) > (b) ? (a) : (b))
#define HDRG_DF_ID        "hdrg.defringe"
#define HDRG_DF_LICENSE   "Apache-2.0 OR MIT"
#define HDRG_DF_COPY      "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_DF_RATIO     2.0f /* the ringing (compression) above which a frame is worked on ... */
#define HDRG_DF_LEVEL     (0.8f / 255.0f) /* ... and its level */
#define HDRG_DF_EDGE      (24 * 257)      /* a 3 x 3 Y' range from which a sample is on an edge */
#define HDRG_DF_LUMA      (6 * 257) /* the neighbors' largest Y' difference (on an edge: half) */
#define HDRG_DF_CB        15260     /* and Cb difference, 32 codes, as B' - Y' (x 1.8556) */
#define HDRG_DF_CR        12951     /* and Cr difference, 32 codes, as R' - Y' (x 1.5748) */
#define HDRG_DF_KR        13933     /* BT.709 luma weights in 1/65536 */
#define HDRG_DF_KG        46871
#define HDRG_DF_KB        4732

/*
 * Defringing of the PREFILTER stage, after the deringing: the false color a
 * coarse or subsampled chroma leaves around edges (a field's color bled
 * across its outline, chroma ringing, which 4:2:0 spreads over 16 x 16) on
 * R'G'B' that arrives with its chroma already upsampled. Within reach - 1
 * samples of a luma edge (a 3 x 3 Y' range of HDRG_DF_EDGE), each sample's
 * chroma becomes the mean chroma of the samples of its window (radius
 * reach = 1 + 2 x strength) within HDRG_DF_LUMA of its Y' (on an edge itself
 * half of it) and within 32 codes of its Cb and Cr: the same field's chroma
 * from a little farther off, where the bleeding has not reached, while
 * another field's (a different Y', or a different color of the same Y') stays
 * out. Y' stays: the chroma is averaged as B' - Y' and R' - Y' and G' follows
 * from them. A frame without the ringing of a coarse quantizer in its
 * HDRG_SD_NOISE side data (as hdrg.dering reads it) passes (reported as
 * passed through). Integer arithmetic on the 16-bit codes, each output sample
 * from the input alone, so any band split or worker count gives the same
 * output.
 */
typedef struct {
    uint32_t width, height, reach;
    bool active;
    double strength;
} hdrg_defringe_t;

/* Y' of the sample at s */
static inline int32_t hdrg_df_luma(const uint16_t *s)
{
    return (int32_t)(((uint32_t)s[0] * HDRG_DF_KR + (uint32_t)s[1] * HDRG_DF_KG +
                      (uint32_t)s[2] * HDRG_DF_KB + 32768u) >>
                     16);
}

/* sum / count rounded half away from zero */
static inline int32_t hdrg_df_mean(int32_t sum, int32_t count)
{
    return (sum >= 0) ? (sum + count / 2) / count : -((-sum + count / 2) / count);
}

static inline uint16_t hdrg_df_code(int32_t v)
{
    return (uint16_t)((v < 0) ? 0 : ((v > 65535) ? 65535 : v));
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_df_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_defringe_t *df;
    hdrg_status_t status;

    (void)filter;

    df = (hdrg_defringe_t *)api->mem_calloc(api, 1, sizeof(hdrg_defringe_t));
    if (df == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &df->strength);
    if (status != HDRG_OK) {
        api->mem_free(api, df);

        return status;
    }

    *inst = df;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_df_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_df_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_defringe_t *df = (hdrg_defringe_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &df->width, &df->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || df->width == 0 || df->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_defringe_settings(nego, api, df->strength, &df->reach, &df->active);
    if (status != HDRG_OK) {
        return status;
    }

    status = api->nego_set_output(nego, df->width, df->height, HDRG_FORMAT_RGB16);

    return (status == HDRG_OK && !df->active) ? api->nego_set_passthrough(nego) : status;
}

/* the window's rows on each side of the output rows */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_df_input_rows(void *inst, uint32_t out_begin,
                                                         uint32_t out_end, uint32_t *in_begin,
                                                         uint32_t *in_end,
                                                         const hdrg_plugin_api_t *api)
{
    const hdrg_defringe_t *df = (const hdrg_defringe_t *)inst;

    (void)api;

    if (out_begin >= out_end || out_end > df->height) {
        return HDRG_ERR_INVALID_ARG;
    }

    *in_begin = (out_begin > df->reach) ? out_begin - df->reach : 0u;
    *in_end = HDRG_DF_MIN(out_end + df->reach, df->height);

    return HDRG_OK;
}

/*
 * The planes of the band's input rows: Y', B' - Y', R' - Y' (int32), whether
 * a sample is on an edge, and that dilated across reach - 1 columns
 */
typedef struct {
    int32_t *luma, *db, *dr;
    uint8_t *edge, *wide;
} hdrg_df_planes_t;

/* the planes of input rows [0, rows) of in (stride bytes a row) */
static void hdrg_df_fill(const hdrg_defringe_t *df, const uint8_t *in, size_t stride, uint32_t rows,
                         const hdrg_df_planes_t *pl)
{
    const uint16_t *s;
    const int32_t *up, *mid, *down;
    uint32_t r, x, a, lo_x, hi_x, side;
    int32_t lo, hi, v;
    size_t i;

    for (r = 0; r < rows; r++) {
        s = (const uint16_t *)(const void *)(in + (size_t)r * stride);

        for (x = 0; x < df->width; x++) {
            i = (size_t)r * df->width + x;
            pl->luma[i] = hdrg_df_luma(s + (size_t)x * 3u);
            pl->db[i] = (int32_t)s[(size_t)x * 3u + 2u] - pl->luma[i];
            pl->dr[i] = (int32_t)s[(size_t)x * 3u] - pl->luma[i];
        }
    }

    side = df->reach - 1u;

    for (r = 0; r < rows; r++) {
        up = pl->luma + (size_t)((r > 0u) ? r - 1u : 0u) * df->width;
        mid = pl->luma + (size_t)r * df->width;
        down = pl->luma + (size_t)HDRG_DF_MIN(r + 1u, rows - 1u) * df->width;

        for (x = 0; x < df->width; x++) {
            lo = INT32_MAX;
            hi = INT32_MIN;

            for (a = (x > 0u) ? x - 1u : 0u; a <= HDRG_DF_MIN(x + 1u, df->width - 1u); a++) {
                v = HDRG_DF_MIN(HDRG_DF_MIN(up[a], mid[a]), down[a]);
                lo = HDRG_DF_MIN(lo, v);
                v = HDRG_DF_MAX(HDRG_DF_MAX(up[a], mid[a]), down[a]);
                hi = HDRG_DF_MAX(hi, v);
            }

            pl->edge[(size_t)r * df->width + x] = (uint8_t)((hi - lo >= HDRG_DF_EDGE) ? 1u : 0u);
        }

        for (x = 0; x < df->width; x++) {
            lo_x = (x > side) ? x - side : 0u;
            hi_x = HDRG_DF_MIN(x + side, df->width - 1u);
            pl->wide[(size_t)r * df->width + x] = 0;

            for (a = lo_x; a <= hi_x; a++) {
                pl->wide[(size_t)r * df->width + x] |= pl->edge[(size_t)r * df->width + a];
            }
        }
    }
}

/* output row y (row r of the planes, rows of them) into out, from the input row in */
static void hdrg_df_row(const hdrg_defringe_t *df, const hdrg_df_planes_t *pl, uint32_t rows,
                        uint32_t r, const uint16_t *in, uint16_t *out)
{
    const uint32_t side = df->reach - 1u;
    uint32_t x, a, b, r0, r1, w0, w1;
    int32_t luma, db, dr, sum_b, sum_r, count, range, mb, mr;
    size_t i, j;
    bool near;

    memcpy(out, in, (size_t)df->width * 3u * sizeof(uint16_t));

    r0 = (r > df->reach) ? r - df->reach : 0u;
    r1 = HDRG_DF_MIN(r + df->reach, rows - 1u);

    for (x = 0; x < df->width; x++) {
        near = false;

        for (b = (r > side) ? r - side : 0u; b <= HDRG_DF_MIN(r + side, rows - 1u) && !near; b++) {
            near = pl->wide[(size_t)b * df->width + x] != 0;
        }

        if (!near) {
            continue;
        }

        i = (size_t)r * df->width + x;
        luma = pl->luma[i];
        db = pl->db[i];
        dr = pl->dr[i];
        range = (pl->edge[i] != 0) ? HDRG_DF_LUMA / 2 : HDRG_DF_LUMA;
        w0 = (x > df->reach) ? x - df->reach : 0u;
        w1 = HDRG_DF_MIN(x + df->reach, df->width - 1u);
        sum_b = 0;
        sum_r = 0;
        count = 0;

        for (b = r0; b <= r1; b++) {
            for (a = w0; a <= w1; a++) {
                j = (size_t)b * df->width + a;

                if (pl->luma[j] - luma > range || luma - pl->luma[j] > range ||
                    pl->db[j] - db > HDRG_DF_CB || db - pl->db[j] > HDRG_DF_CB ||
                    pl->dr[j] - dr > HDRG_DF_CR || dr - pl->dr[j] > HDRG_DF_CR) {
                    continue;
                }

                sum_b += pl->db[j];
                sum_r += pl->dr[j];
                count++;
            }
        }

        mb = hdrg_df_mean(sum_b, count);
        mr = hdrg_df_mean(sum_r, count);

        /* an unchanged chroma keeps its bytes (G' from a rounded Y' could move by one) */
        if (mb == db && mr == dr) {
            continue;
        }

        out[(size_t)x * 3u] = hdrg_df_code(luma + mr);
        out[(size_t)x * 3u + 1u] =
            hdrg_df_code(luma - hdrg_df_mean(HDRG_DF_KR * mr + HDRG_DF_KB * mb, HDRG_DF_KG));
        out[(size_t)x * 3u + 2u] = hdrg_df_code(luma + mb);
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_df_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_defringe_t *df = (const hdrg_defringe_t *)inst;
    const uint8_t *in;
    hdrg_df_planes_t pl;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, rows;
    uint8_t *out;
    size_t in_stride, out_stride, samples;
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

    rows = in_end - in_begin;
    samples = (size_t)rows * df->width;
    status = api->job_scratch(job, samples * 3u * sizeof(int32_t) + samples * 2u, &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    pl.luma = (int32_t *)scratch;

    pl.db = pl.luma + samples;
    pl.dr = pl.db + samples;
    pl.edge = (uint8_t *)(pl.dr + samples);
    pl.wide = pl.edge + samples;
    hdrg_df_fill(df, in, in_stride, rows, &pl);

    for (y = begin; y < end; y++) {
        hdrg_df_row(df, &pl, rows, y - in_begin,
                    (const uint16_t *)(const void *)(in + (size_t)(y - in_begin) * in_stride),
                    (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride));
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_defringe_init(hdrg_plugin_t *plugin,
                                                                const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_DF_ID), HDRG_LIT("Defringing"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_DF_LICENSE), HDRG_LIT(HDRG_DF_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_DF_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_LIT("strength"), 3.0, 0.0, 3.0,
        HDRG_LIT("sets the window's radius, 1 + 2 x strength (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_df_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_df_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_df_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_df_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_df_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_defringe_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_defringe_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_defringe_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}

extern hdrg_status_t hdrg_defringe_settings(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                            double strength, uint32_t *reach, bool *active)
{
    hdrg_status_t status;
    uint32_t version;
    size_t needed;
    float noise[HDRG_SD_NOISE_FLOATS];

    *reach = 1u + (uint32_t)(2.0 * strength + 0.5);
    *active = false;
    status = api->nego_side_data(nego, HDRG_SD_NOISE, &version, noise, sizeof(noise), &needed);

    if (status == HDRG_OK && version == HDRG_SD_NOISE_VERSION && needed == sizeof(noise)) {
        *active = noise[13] > HDRG_DF_RATIO && noise[14] > HDRG_DF_LEVEL && strength > 0.0;
    } else if (status != HDRG_OK && status != HDRG_ERR_NOT_FOUND) {
        return status;
    }

    return HDRG_OK;
}
