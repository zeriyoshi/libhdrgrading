/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_DN_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HDRG_DN_MAX(a, b) ((a) > (b) ? (a) : (b))
#define HDRG_DN_ID        "hdrg.denoise.spatial"
#define HDRG_DN_LICENSE   "Apache-2.0 OR MIT"
#define HDRG_DN_COPY      "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_DN_BINS      HDRG_DENOISE_BINS
#define HDRG_DN_TOP       65535.0
#define HDRG_DN_KR        0.2126 /* the decorrelating Y'CbCr of the code values (BT.709) */
#define HDRG_DN_KB        0.0722
#define HDRG_DN_KG        (1.0 - HDRG_DN_KR - HDRG_DN_KB)
#define HDRG_DN_EPS_MIN   1e-6 /* squared 16-bit codes: a = var / (var + eps) stays defined */

/*
 * Spatial noise reduction of the PREFILTER stage: a
 * self-guided filter (He et al.) on each component of the code values'
 * Y'CbCr, q = mean(a) I + mean(b) with a = var / (var + eps) over
 * (2 radius + 1)^2 windows (cut at the frame's edges). eps is the squared
 * noise level times the strength: Y' takes the noise level function at the
 * window's mean level, Cb / Cr their own level times chroma. Variation at
 * the noise level is smoothed; edges and texture well above it keep a ~ 1.
 * The noise comes from the frame's HDRG_SD_NOISE side data; without it (or
 * a frame too clean to measure) the input passes unchanged (reported as
 * passed through). Sums run in a
 * fixed order in double, so any band split or worker count gives the same
 * output.
 */
typedef struct {
    uint32_t width, height, radius;
    bool active;                /* there is noise to remove */
    double level[HDRG_DN_BINS]; /* Y' noise by level, 16-bit codes; < 0 = no data */
    double sigma, chroma_sigma[2];
    double strength, chroma;
} hdrg_denoise_t;

/* the job's scratch: the input rows' components and the coefficient rows */
typedef struct {
    double *comp[3]; /* Y', Cb, Cr of the input rows */
    double *out[3];  /* filtered components of the output rows */
    double *a, *b, *sum1, *sum2;
} hdrg_dn_work_t;

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_dn_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_denoise_t *dn;
    hdrg_status_t status;
    int64_t radius;

    (void)filter;

    dn = (hdrg_denoise_t *)api->mem_calloc(api, 1, sizeof(hdrg_denoise_t));
    if (dn == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &dn->strength);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_LIT("chroma"), &dn->chroma);
    }

    if (status == HDRG_OK) {
        status = api->option_get_int(options, HDRG_LIT("radius"), &radius);
    }

    if (status != HDRG_OK) {
        api->mem_free(api, dn);

        return status;
    }

    dn->radius = (uint32_t)radius;
    *inst = dn;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_dn_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_dn_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_denoise_t *dn = (hdrg_denoise_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &dn->width, &dn->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || dn->width == 0 || dn->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_denoise_settings(nego, api, dn->strength, &dn->sigma, dn->level, dn->chroma_sigma,
                                   &dn->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, dn->width, dn->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !dn->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_dn_input_rows(void *inst, uint32_t out_begin,
                                                         uint32_t out_end, uint32_t *in_begin,
                                                         uint32_t *in_end,
                                                         const hdrg_plugin_api_t *api)
{
    const hdrg_denoise_t *dn = (const hdrg_denoise_t *)inst;
    const uint32_t reach = 2u * dn->radius;

    (void)api;

    if (out_begin >= out_end || out_end > dn->height) {
        return HDRG_ERR_INVALID_ARG;
    }

    *in_begin = (out_begin > reach) ? out_begin - reach : 0u;
    *in_end = HDRG_DN_MIN(out_end + reach, dn->height);

    return HDRG_OK;
}

/*
 * box means of rows [y0, y1) of src (rows from src_row0, width w) into
 * mean, over windows cut at [0, height): column sums first, then across
 */
static void hdrg_dn_box(const hdrg_denoise_t *dn, const double *src, uint32_t src_row0,
                        uint32_t src_row1, uint32_t y0, uint32_t y1, double *col, double *mean)
{
    const uint32_t w = dn->width, r = dn->radius;
    uint32_t y, x, yy, top, bottom, left, right;
    double s;

    for (y = y0; y < y1; y++) {
        top = HDRG_DN_MAX((y > r) ? y - r : 0u, src_row0);
        bottom = HDRG_DN_MIN(y + r + 1u, src_row1);

        for (x = 0; x < w; x++) {
            s = 0.0;

            for (yy = top; yy < bottom; yy++) {
                s += src[(size_t)(yy - src_row0) * w + x];
            }

            col[x] = s;
        }

        for (x = 0; x < w; x++) {
            left = (x > r) ? x - r : 0u;
            right = HDRG_DN_MIN(x + r + 1u, w);
            s = 0.0;

            for (yy = left; yy < right; yy++) {
                s += col[yy];
            }

            mean[(size_t)(y - y0) * w + x] = s / (double)((bottom - top) * (right - left));
        }
    }
}

/* component k of the output rows [begin, end) from the input rows [in_begin, in_end) */
static void hdrg_dn_component(const hdrg_denoise_t *dn, hdrg_dn_work_t *work, uint8_t k,
                              uint32_t begin, uint32_t end, uint32_t in_begin, uint32_t in_end)
{
    const uint32_t w = dn->width, r = dn->radius, c0 = (begin > r) ? begin - r : 0u,
                   c1 = HDRG_DN_MIN(end + r, dn->height);
    const double *comp = work->comp[k];
    uint32_t y, x;
    size_t i, n;
    double *sq, mean, var, sigma, eps;

    /* the window means of I and I^2 at the coefficient rows [c0, c1) */
    n = (size_t)(in_end - in_begin) * w;
    sq = work->out[k];

    hdrg_dn_box(dn, comp, in_begin, in_end, c0, c1, work->sum2, work->sum1);

    for (i = 0; i < n; i++) {
        work->a[i] = comp[i] * comp[i];
    }

    hdrg_dn_box(dn, work->a, in_begin, in_end, c0, c1, work->sum2, work->b);

    for (i = 0; i < (size_t)(c1 - c0) * w; i++) {
        mean = work->sum1[i];
        var = HDRG_DN_MAX(work->b[i] - mean * mean, 0.0);
        sigma = (k == 0)
                    ? hdrg_denoise_level(dn->level, dn->sigma, mean / HDRG_DN_TOP) * dn->strength
                    : dn->chroma_sigma[k - 1u] * dn->strength * dn->chroma;
        eps = HDRG_DN_MAX(sigma * sigma, HDRG_DN_EPS_MIN);
        work->a[i] = var / (var + eps);
        work->b[i] = mean * (1.0 - work->a[i]);
    }

    /* q = mean(a) I + mean(b) at the output rows */
    hdrg_dn_box(dn, work->a, c0, c1, begin, end, work->sum2, work->sum1);
    hdrg_dn_box(dn, work->b, c0, c1, begin, end, work->sum2, sq + (size_t)(end - begin) * w);

    for (y = begin; y < end; y++) {
        for (x = 0; x < w; x++) {
            i = (size_t)(y - begin) * w + x;
            sq[i] = work->sum1[i] * comp[(size_t)(y - in_begin) * w + x] +
                    sq[(size_t)(end - begin) * w + i];
        }
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_dn_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_denoise_t *dn = (const hdrg_denoise_t *)inst;
    const uint16_t *src;
    const uint8_t *in;
    hdrg_dn_work_t work;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, x, rows, coef_rows;
    uint16_t *dst;
    uint8_t *out, k;
    size_t in_stride, out_stride, i, plane, coef;
    double *at, r, g, b, yy, cb, cr, v[3];
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
    coef_rows = HDRG_DN_MIN(end + dn->radius, dn->height) -
                ((begin > dn->radius) ? begin - dn->radius : 0u);
    /*
     * the job's planes from its scratch: the components, the two outputs, a and b, the sums
     * (all doubles)
     */
    plane = (size_t)rows * dn->width;
    coef = (size_t)HDRG_DN_MAX(rows, coef_rows) * dn->width;
    status = api->job_scratch(job,
                              (3u * plane + 6u * (size_t)(end - begin) * dn->width + 2u * coef +
                               (size_t)coef_rows * dn->width + dn->width) *
                                  sizeof(double),
                              &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    memset(&work, 0, sizeof(work));
    at = (double *)scratch;

    for (k = 0; k < 3u; k++) {
        work.comp[k] = at;
        at += plane;
        work.out[k] = at;
        at += (size_t)(end - begin) * 2u * dn->width;
    }

    work.a = at;
    work.b = at + coef;
    work.sum1 = at + 2u * coef;
    work.sum2 = work.sum1 + (size_t)coef_rows * dn->width;

    /* zeroed: GCC cannot tell the squares fill the rows the box means read */
    memset(work.a, 0, coef * sizeof(double));

    for (y = 0; y < rows; y++) {
        src = (const uint16_t *)(const void *)(in + (size_t)y * in_stride);

        for (x = 0; x < dn->width; x++) {
            i = (size_t)y * dn->width + x;
            r = (double)src[(size_t)x * 3u];
            g = (double)src[(size_t)x * 3u + 1u];
            b = (double)src[(size_t)x * 3u + 2u];
            yy = HDRG_DN_KR * r + HDRG_DN_KG * g + HDRG_DN_KB * b;
            work.comp[0][i] = yy;
            work.comp[1][i] = (b - yy) / (2.0 * (1.0 - HDRG_DN_KB));
            work.comp[2][i] = (r - yy) / (2.0 * (1.0 - HDRG_DN_KR));
        }
    }

    for (k = 0; k < 3u; k++) {
        hdrg_dn_component(dn, &work, k, begin, end, in_begin, in_end);
    }

    for (y = begin; y < end; y++) {
        dst = (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride);

        for (x = 0; x < dn->width; x++) {
            i = (size_t)(y - begin) * dn->width + x;
            yy = work.out[0][i];
            cb = work.out[1][i];
            cr = work.out[2][i];
            v[0] = yy + 2.0 * (1.0 - HDRG_DN_KR) * cr;
            v[2] = yy + 2.0 * (1.0 - HDRG_DN_KB) * cb;
            v[1] = (yy - HDRG_DN_KR * v[0] - HDRG_DN_KB * v[2]) / HDRG_DN_KG;

            for (k = 0; k < 3u; k++) {
                v[k] = floor(v[k] + 0.5);
                dst[(size_t)x * 3u + k] =
                    (uint16_t)((v[k] < 0.0) ? 0.0 : ((v[k] > HDRG_DN_TOP) ? HDRG_DN_TOP : v[k]));
            }
        }
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_denoise_init(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_DN_ID), HDRG_LIT("Spatial denoise"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_DN_LICENSE), HDRG_LIT(HDRG_DN_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_DN_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_LIT("strength"), 1.0, 0.0, 4.0,
        HDRG_LIT("smoothing threshold over the measured noise level (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_LIT("chroma"), 2.0, 0.0, 4.0,
            HDRG_LIT("the chroma threshold over the luma's (chroma noise reads worse)"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(cls, HDRG_LIT("radius"), 2, 1, 4,
                                           HDRG_LIT("window radius in pixels"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_dn_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_dn_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_dn_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_dn_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_dn_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_denoise_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_denoise_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_denoise_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}
