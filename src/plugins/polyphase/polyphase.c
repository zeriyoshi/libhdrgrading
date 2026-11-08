/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "parts.h"

#define HDRG_LIT(s)        s, sizeof(s) - 1u
#define HDRG_PP_MIN(a, b)  ((a) < (b) ? (a) : (b))
#define HDRG_PP_MAX(a, b)  ((a) > (b) ? (a) : (b))
#define HDRG_PP_SHIFT      HDRG_POLYPHASE_SHIFT /* weights in Q14 */
#define HDRG_PP_ONE        (INT32_C(1) << HDRG_PP_SHIFT)
#define HDRG_PP_EXTRA      HDRG_POLYPHASE_EXTRA /* the vertical pass keeps two more bits */
#define HDRG_PP_AR_SHIFT   HDRG_POLYPHASE_AR    /* anti-ringing strength in Q8 */
#define HDRG_PP_LANCZOS    0u
#define HDRG_PP_BICUBIC    1u
#define HDRG_PP_PLUGIN     "hdrg.scale.polyphase"
#define HDRG_LZ_ID         "hdrg.scale.lanczos"
#define HDRG_BC_ID         "hdrg.scale.bicubic"
#define HDRG_PP_LICENSE    "Apache-2.0 OR MIT"
#define HDRG_PP_COPY       "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_PP_PI2        9.869604401089358 /* pi^2 */
#define HDRG_PP_AR_DEFAULT 1.0

/*
 * Separable polyphase resampling of RGB16 code values:
 * Lanczos (2 to 4 lobes) or the Mitchell-Netravali bicubic family, pixel
 * centers aligned, the kernel stretched by the reduction when downscaling
 * (no aliasing). Each output sample's taps and Q14 weights (summing to one
 * exactly, edges repeated) are tabulated at configure from the common layer's
 * deterministic sine; the passes are integer arithmetic, so every compiler
 * and band split gives the same output. When upscaling an axis, the result
 * is pulled toward the range of the two source samples around it
 * (anti-ringing: the HDR boost would show the overshoot up to five times
 * brighter); vertical pass first, at the source width.
 */
typedef struct {
    hdrg_polyphase_axis_t cols, rows;
    uint32_t in_w, in_h, out_w, out_h;
    int32_t antiring; /* Q8 */
    uint8_t kind;     /* HDRG_PP_LANCZOS / HDRG_PP_BICUBIC */
    uint8_t lobes;    /* Lanczos */
    double b, c;      /* bicubic */
} hdrg_polyphase_t;

/* floor(v / 2^shift) for either sign */
static inline int64_t hdrg_pp_shr(int64_t v, uint8_t shift)
{
    return (v >= 0) ? v >> shift : -((-v + ((INT64_C(1) << shift) - 1)) >> shift);
}

/* v pulled toward [lo, hi] by the anti-ringing strength */
static inline int64_t hdrg_pp_antiring(int64_t v, int64_t lo, int64_t hi, int32_t strength)
{
    const int64_t clamped = (v < lo) ? lo : ((v > hi) ? hi : v);

    return v + hdrg_pp_shr((clamped - v) * strength + (INT64_C(1) << (HDRG_PP_AR_SHIFT - 1u)),
                           HDRG_PP_AR_SHIFT);
}

/* the kernel: Lanczos of lobes, or (lobes 0) the bicubic of b and c */
static double hdrg_pp_kernel(uint8_t lobes, double b, double c, double x)
{
    const double a = (double)lobes;

    x = fabs(x);

    if (lobes > 0) {
        if (x == 0.0) {
            return 1.0;
        }

        if (x >= a) {
            return 0.0;
        }

        return a * (double)hdrg_math_sinpif((float)x) * (double)hdrg_math_sinpif((float)(x / a)) /
               (HDRG_PP_PI2 * x * x);
    }

    if (x < 1.0) {
        return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x + (-18.0 + 12.0 * b + 6.0 * c) * x * x +
                (6.0 - 2.0 * b)) /
               6.0;
    }

    if (x < 2.0) {
        return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x +
                (-12.0 * b - 48.0 * c) * x + (8.0 * b + 24.0 * c)) /
               6.0;
    }

    return 0.0;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_pp_create(void **inst, uint8_t kind,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_polyphase_t *pp;

    pp = (hdrg_polyphase_t *)api->mem_calloc(api, 1, sizeof(hdrg_polyphase_t));
    if (pp == NULL) {
        return HDRG_ERR_NOMEM;
    }

    pp->kind = kind;
    *inst = pp;

    return HDRG_OK;
}

static hdrg_status_t hdrg_pp_antiring_option(hdrg_polyphase_t *pp, const hdrg_options_t *options,
                                             const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;
    double strength;

    status = api->option_get_float(options, HDRG_LIT("antiring"), &strength);
    if (status != HDRG_OK) {
        return status;
    }

    pp->antiring = (int32_t)floor(strength * (double)(1u << HDRG_PP_AR_SHIFT) + 0.5);

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_lz_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_polyphase_t *pp;
    hdrg_status_t status;
    int64_t lobes;

    (void)filter;

    status = hdrg_pp_create(inst, HDRG_PP_LANCZOS, api);
    if (status != HDRG_OK) {
        return status;
    }

    pp = (hdrg_polyphase_t *)*inst;
    status = api->option_get_int(options, HDRG_LIT("taps"), &lobes);

    if (status == HDRG_OK) {
        pp->lobes = (uint8_t)lobes;
        status = hdrg_pp_antiring_option(pp, options, api);
    }

    if (status != HDRG_OK) {
        api->mem_free(api, pp);
        *inst = NULL;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_bc_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_polyphase_t *pp;
    hdrg_status_t status;

    (void)filter;

    status = hdrg_pp_create(inst, HDRG_PP_BICUBIC, api);
    if (status != HDRG_OK) {
        return status;
    }

    pp = (hdrg_polyphase_t *)*inst;
    status = api->option_get_float(options, HDRG_LIT("b"), &pp->b);

    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_LIT("c"), &pp->c);
    }

    if (status == HDRG_OK) {
        status = hdrg_pp_antiring_option(pp, options, api);
    }

    if (status != HDRG_OK) {
        api->mem_free(api, pp);
        *inst = NULL;
    }

    return status;
}

static void HDRG_PLUGIN_CALL hdrg_pp_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_polyphase_t *pp = (hdrg_polyphase_t *)inst;

    hdrg_polyphase_axis_free(api, &pp->cols);
    hdrg_polyphase_axis_free(api, &pp->rows);
    api->mem_free(api, pp);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_pp_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_polyphase_t *pp = (hdrg_polyphase_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &pp->in_w, &pp->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &pp->out_w, &pp->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || pp->in_w == 0 || pp->in_h == 0 || pp->out_w == 0 ||
        pp->out_h == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    /* the tables, kept while the sizes stay */
    if (pp->cols.n != pp->in_w || pp->cols.m != pp->out_w) {
        status = hdrg_polyphase_axis(api, (pp->kind == HDRG_PP_LANCZOS) ? pp->lobes : 0u, pp->b,
                                     pp->c, pp->in_w, pp->out_w, &pp->cols);
    }

    if (status == HDRG_OK && (pp->rows.n != pp->in_h || pp->rows.m != pp->out_h)) {
        status = hdrg_polyphase_axis(api, (pp->kind == HDRG_PP_LANCZOS) ? pp->lobes : 0u, pp->b,
                                     pp->c, pp->in_h, pp->out_h, &pp->rows);
    }

    if (status != HDRG_OK) {
        return status;
    }

    return api->nego_set_output(nego, pp->out_w, pp->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_pp_input_rows(void *inst, uint32_t out_begin,
                                                         uint32_t out_end, uint32_t *in_begin,
                                                         uint32_t *in_end,
                                                         const hdrg_plugin_api_t *api)
{
    const hdrg_polyphase_t *pp = (const hdrg_polyphase_t *)inst;
    const uint32_t taps = pp->rows.taps;

    (void)api;

    if (out_begin >= out_end || out_end > pp->out_h) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* the taps of a row are nondecreasing, and so are the rows' */
    *in_begin = pp->rows.index[(size_t)out_begin * taps];
    *in_end = pp->rows.index[(size_t)(out_end - 1u) * taps + taps - 1u] + 1u;

    return HDRG_OK;
}

/* output row y at the source width, Q2: the vertical taps (and anti-ringing) */
static void hdrg_pp_vertical(const hdrg_polyphase_t *pp, const uint8_t *in, size_t stride,
                             uint32_t in_begin, uint32_t y, int32_t *line)
{
    const uint32_t taps = pp->rows.taps, *index = pp->rows.index + (size_t)y * taps;
    const uint16_t *src, *a, *b;
    const int32_t *weight = pp->rows.weight + (size_t)y * taps;
    uint32_t x, k;
    int64_t acc, lo, hi;

    a = (const uint16_t *)(const void *)(in + (size_t)(pp->rows.near[y * 2u] - in_begin) * stride);
    b = (const uint16_t *)(const void *)(in +
                                         (size_t)(pp->rows.near[y * 2u + 1u] - in_begin) * stride);

    for (x = 0; x < pp->in_w * 3u; x++) {
        acc = 0;

        for (k = 0; k < taps; k++) {
            src = (const uint16_t *)(const void *)(in + (size_t)(index[k] - in_begin) * stride);
            acc += (int64_t)weight[k] * (int64_t)src[x];
        }

        acc = hdrg_pp_shr(acc + (INT64_C(1) << (HDRG_PP_SHIFT - HDRG_PP_EXTRA - 1u)),
                          HDRG_PP_SHIFT - HDRG_PP_EXTRA);

        if (pp->rows.upscale && pp->antiring > 0) {
            lo = (int64_t)HDRG_PP_MIN(a[x], b[x]) << HDRG_PP_EXTRA;
            hi = (int64_t)HDRG_PP_MAX(a[x], b[x]) << HDRG_PP_EXTRA;
            acc = hdrg_pp_antiring(acc, lo, hi, pp->antiring);
        }

        line[x] = (int32_t)acc;
    }
}

/* the horizontal taps (and anti-ringing) of a vertical line into a row of code values */
static void hdrg_pp_horizontal(const hdrg_polyphase_t *pp, const int32_t *line, uint16_t *dst)
{
    const uint32_t taps = pp->cols.taps, *index;
    const int32_t *weight;
    uint32_t x, k, a, b;
    uint8_t c;
    int64_t acc, lo, hi;

    for (x = 0; x < pp->out_w; x++) {
        index = pp->cols.index + (size_t)x * taps;
        weight = pp->cols.weight + (size_t)x * taps;
        a = pp->cols.near[x * 2u] * 3u;
        b = pp->cols.near[x * 2u + 1u] * 3u;

        for (c = 0; c < 3u; c++) {
            acc = 0;

            for (k = 0; k < taps; k++) {
                acc += (int64_t)weight[k] * (int64_t)line[index[k] * 3u + c];
            }

            acc = hdrg_pp_shr(acc + (INT64_C(1) << (HDRG_PP_SHIFT - 1u)), HDRG_PP_SHIFT);

            if (pp->cols.upscale && pp->antiring > 0) {
                lo = HDRG_PP_MIN(line[a + c], line[b + c]);
                hi = HDRG_PP_MAX(line[a + c], line[b + c]);
                acc = hdrg_pp_antiring(acc, lo, hi, pp->antiring);
            }

            acc = hdrg_pp_shr(acc + (INT64_C(1) << (HDRG_PP_EXTRA - 1u)), HDRG_PP_EXTRA);
            dst[(size_t)x * 3u + c] = (uint16_t)((acc < 0) ? 0 : ((acc > 65535) ? 65535 : acc));
        }
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_pp_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_polyphase_t *pp = (const hdrg_polyphase_t *)inst;
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, first, last;
    uint8_t *out;
    int32_t *line;
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

    first = pp->rows.index[(size_t)begin * pp->rows.taps];
    last = pp->rows.index[(size_t)(end - 1u) * pp->rows.taps + pp->rows.taps - 1u];
    if (first < in_begin || last >= in_end) {
        return HDRG_ERR_INTERNAL;
    }

    status = api->job_scratch(job, (size_t)pp->in_w * 3u * sizeof(int32_t), &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    line = (int32_t *)scratch;

    for (y = begin; y < end; y++) {
        hdrg_pp_vertical(pp, in, in_stride, in_begin, y, line);
        hdrg_pp_horizontal(pp, line, (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride));
    }

    return HDRG_OK;
}

/* the callbacks both classes share, then the registration */
static hdrg_status_t hdrg_pp_register(hdrg_class_t *cls, hdrg_filter_create_fn_t create,
                                      const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->class_add_option_float(
        cls, HDRG_LIT("antiring"), HDRG_PP_AR_DEFAULT, 0.0, 1.0,
        HDRG_LIT("when upscaling, pull each sample toward the range of the source samples around "
                 "it (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_pp_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_pp_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_pp_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_pp_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}

extern void hdrg_polyphase_axis_free(const hdrg_plugin_api_t *api, hdrg_polyphase_axis_t *axis)
{
    api->mem_free(api, axis->index);
    api->mem_free(api, axis->weight);
    api->mem_free(api, axis->near);
    memset(axis, 0, sizeof(*axis));
}

extern hdrg_status_t hdrg_polyphase_axis(const hdrg_plugin_api_t *api, uint8_t lobes, double b,
                                         double c, uint32_t n, uint32_t m,
                                         hdrg_polyphase_axis_t *axis)
{
    const double stretch = HDRG_PP_MAX((double)n / (double)m, 1.0),
                 support = ((lobes > 0) ? (double)lobes : 2.0) * stretch;
    uint32_t i, k, taps, heaviest;
    int64_t first, j;
    int32_t sum;
    double center, total, *w;

    hdrg_polyphase_axis_free(api, axis);

    taps = 2u * (uint32_t)ceil(support);
    w = (double *)api->mem_alloc(api, (size_t)taps * sizeof(double));
    axis->index = (uint32_t *)api->mem_alloc(api, (size_t)m * taps * sizeof(uint32_t));
    axis->weight = (int32_t *)api->mem_alloc(api, (size_t)m * taps * sizeof(int32_t));
    axis->near = (uint32_t *)api->mem_alloc(api, (size_t)m * 2u * sizeof(uint32_t));
    if (w == NULL || axis->index == NULL || axis->weight == NULL || axis->near == NULL) {
        api->mem_free(api, w);
        hdrg_polyphase_axis_free(api, axis);

        return HDRG_ERR_NOMEM;
    }

    axis->taps = taps;
    axis->upscale = m > n;
    axis->n = n;
    axis->m = m;

    for (i = 0; i < m; i++) {
        center = ((double)i + 0.5) * (double)n / (double)m - 0.5;
        first = (int64_t)floor(center) - (int64_t)ceil(support) + 1;
        total = 0.0;
        heaviest = 0;

        for (k = 0; k < taps; k++) {
            w[k] = hdrg_pp_kernel(lobes, b, c, ((double)(first + (int64_t)k) - center) / stretch);
            total += w[k];
            heaviest = (fabs(w[k]) > fabs(w[heaviest])) ? k : heaviest;
        }

        sum = 0;

        for (k = 0; k < taps; k++) {
            j = first + (int64_t)k;
            axis->index[(size_t)i * taps + k] =
                (uint32_t)((j < 0) ? 0 : ((j >= (int64_t)n) ? (int64_t)n - 1 : j));
            axis->weight[(size_t)i * taps + k] =
                (int32_t)floor(w[k] / total * (double)HDRG_PP_ONE + 0.5);
            sum += axis->weight[(size_t)i * taps + k];
        }

        /* the rounding error goes to the heaviest tap: weights sum to one exactly */
        axis->weight[(size_t)i * taps + heaviest] += HDRG_PP_ONE - sum;

        j = (int64_t)floor(center);
        axis->near[i * 2u] = (uint32_t)((j < 0) ? 0 : ((j >= (int64_t)n) ? (int64_t)n - 1 : j));
        j++;
        axis->near[i * 2u + 1u] =
            (uint32_t)((j < 0) ? 0 : ((j >= (int64_t)n) ? (int64_t)n - 1 : j));
    }

    api->mem_free(api, w);

    return HDRG_OK;
}

extern int32_t *hdrg_polyphase_pack(const hdrg_plugin_api_t *api, const hdrg_polyphase_axis_t *rows,
                                    const hdrg_polyphase_axis_t *cols, uint32_t out_w,
                                    uint32_t out_h, uint32_t offsets[6], size_t *count)
{
    const hdrg_polyphase_axis_t *axis;
    const uint32_t samples[2] = {out_h, out_w};
    uint32_t a;
    int32_t *table;
    size_t at, i, n;

    n = 0;

    for (a = 0; a < 2u; a++) {
        axis = (a == 0) ? rows : cols;
        n += (size_t)samples[a] * (2u * axis->taps + 2u);
    }

    table = (int32_t *)api->mem_alloc(api, n * sizeof(int32_t));
    if (table == NULL) {
        return NULL;
    }

    at = 0;

    for (a = 0; a < 2u; a++) {
        axis = (a == 0) ? rows : cols;
        offsets[a * 3u] = (uint32_t)at;

        for (i = 0; i < (size_t)samples[a] * axis->taps; i++) {
            table[at++] = (int32_t)axis->index[i];
        }

        offsets[a * 3u + 1u] = (uint32_t)at;

        for (i = 0; i < (size_t)samples[a] * axis->taps; i++) {
            table[at++] = axis->weight[i];
        }

        offsets[a * 3u + 2u] = (uint32_t)at;

        for (i = 0; i < (size_t)samples[a] * 2u; i++) {
            table[at++] = (int32_t)axis->near[i];
        }
    }

    *count = n;

    return table;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_polyphase_init(hdrg_plugin_t *plugin,
                                                                 const hdrg_plugin_api_t *api)
{
    hdrg_class_t *lanczos, *bicubic;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_PP_PLUGIN), HDRG_LIT("Polyphase scalers"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_PP_LICENSE), HDRG_LIT(HDRG_PP_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    lanczos = api->class_create(plugin, HDRG_LIT(HDRG_LZ_ID));
    if (lanczos == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_int(lanczos, HDRG_LIT("taps"), 3, 2, 4,
                                       HDRG_LIT("lobes of the Lanczos kernel"));
    if (status == HDRG_OK) {
        status = hdrg_pp_register(lanczos, hdrg_lz_create, api);
    }

    if (status != HDRG_OK) {
        return status;
    }

    bicubic = api->class_create(plugin, HDRG_LIT(HDRG_BC_ID));
    if (bicubic == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    /* Catmull-Rom by default; b = c = 1/3 is Mitchell-Netravali */
    status = api->class_add_option_float(bicubic, HDRG_LIT("b"), 0.0, 0.0, 1.0,
                                         HDRG_LIT("Mitchell-Netravali B (blur)"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(bicubic, HDRG_LIT("c"), 0.5, 0.0, 1.0,
                                             HDRG_LIT("Mitchell-Netravali C (ringing)"));
    }

    if (status == HDRG_OK) {
        status = hdrg_pp_register(bicubic, hdrg_bc_create, api);
    }

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_lanczos_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_lanczos_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_lanczos_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}
