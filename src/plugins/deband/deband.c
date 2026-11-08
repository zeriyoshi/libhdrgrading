/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_DB_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HDRG_DB_MAX(a, b) ((a) > (b) ? (a) : (b))
#define HDRG_DB_ID        "hdrg.deband"
#define HDRG_DB_LICENSE   "Apache-2.0 OR MIT"
#define HDRG_DB_COPY      "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_DB_RADII     2u               /* the windows tried, the largest first */
#define HDRG_DB_EDGE      (12 * 257)       /* a 3 x 3 Y' range from which a window has an edge */
#define HDRG_DB_LO        (0.8 * 257.0)    /* the residual RMS of a full weight, x strength */
#define HDRG_DB_HI        (1.6 * 257.0)    /* and of none */
#define HDRG_DB_QUANT     (0.4 * 257.0)    /* a residual RMS left by quantization, x strength ... */
#define HDRG_DB_GRAIN     (0.6667 * 257.0) /* ... from which only steps (coherent) are smoothed */
#define HDRG_DB_COH_LO    0.2              /* the residual's coherence of grain ... */
#define HDRG_DB_COH_HI    0.5              /* ... and of steps */
#define HDRG_DB_KR        13933u           /* BT.709 luma weights in 1/65536 for the edges */
#define HDRG_DB_KG        46871u
#define HDRG_DB_KB        4732u

/*
 * Debanding of the PREFILTER stage, after the defringing: 8-bit gradients
 * (and a coarse quantizer's) come as steps that a highlight expansion makes
 * visible bands of. Each sample takes the value at its position of the
 * plane (a + b x + c y, per channel) fitted to its window by least squares,
 * by a weight from the fit's residual: the steps of a ramp and a light noise
 * leave a small residual (the plane's value is the smooth ramp under them),
 * texture, grain and detail a large one (the samples stay). The windows are
 * 15 x 15 then 9 x 9, the first without a luma edge (a 3 x 3 Y' range of
 * HDRG_DB_EDGE) and with a weight; the weight is 1 up to a residual RMS
 * (the worst channel) of HDRG_DB_LO x strength and 0 from HDRG_DB_HI x
 * strength. Above the residual quantization leaves (HDRG_DB_QUANT x
 * strength, all of it to HDRG_DB_GRAIN x strength: the strength trades the
 * grain kept for the steps smoothed) the window must also hold steps: the
 * coherence of the structure tensor of its Y' central differences less the
 * plane's slope (all across parallel steps: 1; grain, every way: 0) scales
 * the weight from HDRG_DB_COH_LO to HDRG_DB_COH_HI, so grain keeps its
 * bytes evenly (a window found grain is not retried smaller, where the
 * coherence is noisier). A frame whose quietest level of Y' noise
 * (HDRG_SD_NOISE) reaches HDRG_DB_HI x strength has no window to work on
 * and passes (reported as passthrough). The sums of the fits run over the
 * band as exact integers (column sums, then row prefix sums), so any band
 * split or worker count gives the same output.
 */
typedef struct {
    uint32_t width, height, reach;
    bool active;
    double strength;
} hdrg_deband_t;

/*
 * a window's sums of one channel: values, squares, x x value, y x value;
 * its edges; and of its Y' differences (dx, dy): dx^2, dy^2, dx dy, dx, dy
 */
typedef struct {
    int64_t *sum[3], *square[3], *ycol[3];
    int64_t *edge, *grad[5];
} hdrg_db_columns_t;

static const uint32_t hdrg_db_radii[HDRG_DB_RADII] = {7, 4};
static const double hdrg_db_k[3] = {(double)HDRG_DB_KR / 65536.0, (double)HDRG_DB_KG / 65536.0,
                                    (double)HDRG_DB_KB / 65536.0};

/* Y' of the sample at s */
static inline int32_t hdrg_db_luma(const uint16_t *s)
{
    return (int32_t)(((uint32_t)s[0] * HDRG_DB_KR + (uint32_t)s[1] * HDRG_DB_KG +
                      (uint32_t)s[2] * HDRG_DB_KB + 32768u) >>
                     16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_db_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_deband_t *db;
    hdrg_status_t status;

    (void)filter;

    db = (hdrg_deband_t *)api->mem_calloc(api, 1, sizeof(hdrg_deband_t));
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
    hdrg_deband_t *db = (hdrg_deband_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &db->width, &db->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || db->width == 0 || db->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    db->reach = hdrg_db_radii[0] + 1u;
    status = hdrg_deband_settings(nego, api, db->strength, &db->active);
    if (status != HDRG_OK) {
        return status;
    }

    status = api->nego_set_output(nego, db->width, db->height, HDRG_FORMAT_RGB16);

    return (status == HDRG_OK && !db->active) ? api->nego_set_passthrough(nego) : status;
}

/* the largest window's rows and its edges' on each side of the output rows */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_db_input_rows(void *inst, uint32_t out_begin,
                                                         uint32_t out_end, uint32_t *in_begin,
                                                         uint32_t *in_end,
                                                         const hdrg_plugin_api_t *api)
{
    const hdrg_deband_t *db = (const hdrg_deband_t *)inst;

    (void)api;

    if (out_begin >= out_end || out_end > db->height) {
        return HDRG_ERR_INVALID_ARG;
    }

    *in_begin = (out_begin > db->reach) ? out_begin - db->reach : 0u;
    *in_end = HDRG_DB_MIN(out_end + db->reach, db->height);

    return HDRG_OK;
}

/* Y' of the input rows [first, last) into luma (width each) */
static void hdrg_db_lumas(const hdrg_deband_t *db, const uint8_t *in, size_t stride, uint32_t first,
                          uint32_t last, int32_t *luma)
{
    const uint16_t *row;
    uint32_t y, x;

    for (y = first; y < last; y++) {
        row = (const uint16_t *)(const void *)(in + (size_t)(y - first) * stride);

        for (x = 0; x < db->width; x++) {
            luma[(size_t)(y - first) * db->width + x] = hdrg_db_luma(row + (size_t)x * 3u);
        }
    }
}

/*
 * whether each pixel of the input rows [first, last) has an edge (the Y'
 * range of its 3 x 3, clamped to the rows and the frame's columns, reaches
 * HDRG_DB_EDGE), into flags: the ranges across each row (lo, hi: width
 * each per row), then down the rows
 */
static void hdrg_db_edges(const hdrg_deband_t *db, const int32_t *luma, uint32_t first,
                          uint32_t last, int32_t *lo, int32_t *hi, uint8_t *flags)
{
    const uint32_t w = db->width;
    const int32_t *l;
    uint32_t y, x, r, a, b;
    int32_t mn, mx;

    for (y = first; y < last; y++) {
        l = luma + (size_t)(y - first) * w;

        for (x = 0; x < w; x++) {
            a = (x > 0u) ? x - 1u : 0u;
            b = HDRG_DB_MIN(x + 1u, w - 1u);
            mn = HDRG_DB_MIN(HDRG_DB_MIN(l[a], l[x]), l[b]);
            mx = HDRG_DB_MAX(HDRG_DB_MAX(l[a], l[x]), l[b]);
            lo[(size_t)(y - first) * w + x] = mn;
            hi[(size_t)(y - first) * w + x] = mx;
        }
    }

    for (y = first; y < last; y++) {
        for (x = 0; x < w; x++) {
            mn = INT32_MAX;
            mx = INT32_MIN;

            for (r = (y > first) ? y - 1u : y; r <= HDRG_DB_MIN(y + 1u, last - 1u); r++) {
                mn = HDRG_DB_MIN(mn, lo[(size_t)(r - first) * w + x]);
                mx = HDRG_DB_MAX(mx, hi[(size_t)(r - first) * w + x]);
            }

            flags[(size_t)(y - first) * w + x] = (uint8_t)((mx - mn >= HDRG_DB_EDGE) ? 1u : 0u);
        }
    }
}

/*
 * add (sign 1) or remove (-1) image row y to the column sums; the Y'
 * central differences (the frame's edge samples repeated), which share no
 * sample between the axes
 */
static void hdrg_db_column_row(const hdrg_deband_t *db, const uint8_t *in, size_t stride,
                               uint32_t first, uint32_t y, const int32_t *luma,
                               const uint8_t *flags, int64_t sign, const hdrg_db_columns_t *cols)
{
    const uint16_t *row = (const uint16_t *)(const void *)(in + (size_t)(y - first) * stride);
    const int32_t *l = luma + (size_t)(y - first) * db->width, *prev = (y > 0u) ? l - db->width : l,
                  *next = (y + 1u < db->height) ? l + db->width : l;
    uint32_t x;
    uint8_t c;
    int64_t v, dx, dy;

    for (x = 0; x < db->width; x++) {
        for (c = 0; c < 3u; c++) {
            v = (int64_t)row[(size_t)x * 3u + c];
            cols->sum[c][x] += sign * v;
            cols->square[c][x] += sign * v * v;
            cols->ycol[c][x] += sign * v * (int64_t)y;
        }

        dx = (int64_t)l[HDRG_DB_MIN(x + 1u, db->width - 1u)] - (int64_t)l[(x > 0u) ? x - 1u : 0u];
        dy = (int64_t)next[x] - (int64_t)prev[x];
        cols->edge[x] += sign * (int64_t)flags[x];
        cols->grad[0][x] += sign * dx * dx;
        cols->grad[1][x] += sign * dy * dy;
        cols->grad[2][x] += sign * dx * dy;
        cols->grad[3][x] += sign * dx;
        cols->grad[4][x] += sign * dy;
    }
}

/*
 * Output row y: for each window radius its column sums over rows
 * [y - radius, y + radius] (cols[i]) turned into row prefix sums, then the
 * fit per sample
 */
static void hdrg_db_row(const hdrg_deband_t *db, const hdrg_db_columns_t *cols, int64_t *prefix,
                        uint32_t y, const uint16_t *in, uint16_t *out)
{
    const size_t n1 = (size_t)db->width + 1u;
    uint32_t x, x0, x1, r, i;
    uint8_t c, g;
    int64_t *ps[HDRG_DB_RADII][3], *pq[HDRG_DB_RADII][3], *px[HDRG_DB_RADII][3],
        *py[HDRG_DB_RADII][3], *pe[HDRG_DB_RADII], *pg[HDRG_DB_RADII][5], s, q, sx, sy;
    bool fits, grain;
    double n, sxx, worst, plane[3], weight, m, bx, by, var, v, lo, hi, lx, ly, gxx, gyy, gxy, coh,
        steps;

    memcpy(out, in, (size_t)db->width * 3u * sizeof(uint16_t));

    lo = HDRG_DB_LO * db->strength;
    hi = HDRG_DB_HI * db->strength;

    for (i = 0; i < HDRG_DB_RADII; i++) {
        pe[i] = prefix + ((size_t)i * 18u) * n1;
        pe[i][0] = 0;

        for (g = 0; g < 5u; g++) {
            pg[i][g] = pe[i] + (13u + g) * n1;
            pg[i][g][0] = 0;
        }

        for (c = 0; c < 3u; c++) {
            ps[i][c] = pe[i] + (1u + 4u * c) * n1;
            pq[i][c] = ps[i][c] + n1;
            px[i][c] = pq[i][c] + n1;
            py[i][c] = px[i][c] + n1;
            ps[i][c][0] = 0;
            pq[i][c][0] = 0;
            px[i][c][0] = 0;
            py[i][c][0] = 0;
        }

        for (x = 0; x < db->width; x++) {
            pe[i][x + 1u] = pe[i][x] + cols[i].edge[x];

            for (g = 0; g < 5u; g++) {
                pg[i][g][x + 1u] = pg[i][g][x] + cols[i].grad[g][x];
            }

            for (c = 0; c < 3u; c++) {
                ps[i][c][x + 1u] = ps[i][c][x] + cols[i].sum[c][x];
                pq[i][c][x + 1u] = pq[i][c][x] + cols[i].square[c][x];
                px[i][c][x + 1u] = px[i][c][x] + cols[i].sum[c][x] * (int64_t)x;
                py[i][c][x + 1u] = py[i][c][x] + cols[i].ycol[c][x];
            }
        }
    }

    for (x = 0; x < db->width; x++) {
        weight = 0.0;
        grain = false;

        for (i = 0; i < HDRG_DB_RADII && weight <= 0.0 && !grain; i++) {
            r = hdrg_db_radii[i];
            fits = x >= r && y >= r && x + r < db->width && y + r < db->height;

            if (!fits) {
                continue;
            }

            x0 = x - r;
            x1 = x + r + 1u;

            if (pe[i][x1] - pe[i][x0] > 0) {
                continue;
            }

            n = (double)((2u * r + 1u) * (2u * r + 1u));
            sxx = (double)(r * (r + 1u) * (2u * r + 1u)) / 3.0 * (double)(2u * r + 1u);
            worst = 0.0;
            lx = 0.0;
            ly = 0.0;

            for (c = 0; c < 3u; c++) {
                s = ps[i][c][x1] - ps[i][c][x0];
                q = pq[i][c][x1] - pq[i][c][x0];
                sx = (px[i][c][x1] - px[i][c][x0]) - (int64_t)x * s;
                sy = (py[i][c][x1] - py[i][c][x0]) - (int64_t)y * s;
                m = (double)s / n;
                bx = (double)sx / sxx;
                by = (double)sy / sxx;
                var = ((double)q - (double)s * m - bx * (double)sx - by * (double)sy) / n;
                worst = HDRG_DB_MAX(worst, sqrt(HDRG_DB_MAX(var, 0.0)));
                plane[c] = m;
                lx += 2.0 * hdrg_db_k[c] * bx;
                ly += 2.0 * hdrg_db_k[c] * by;
            }

            weight = (hi - worst) / (hi - lo);
            weight = HDRG_DB_MIN(HDRG_DB_MAX(weight, 0.0), 1.0);

            /*
             * above what quantization leaves, only steps: the residual's Y'
             * differences (less the plane's slope) all across parallel steps
             * (coherent), not every way as grain
             */
            if (weight > 0.0 && worst > HDRG_DB_QUANT * db->strength) {
                gxx = (double)(pg[i][0][x1] - pg[i][0][x0]) -
                      2.0 * lx * (double)(pg[i][3][x1] - pg[i][3][x0]) + n * lx * lx;
                gyy = (double)(pg[i][1][x1] - pg[i][1][x0]) -
                      2.0 * ly * (double)(pg[i][4][x1] - pg[i][4][x0]) + n * ly * ly;
                gxy = (double)(pg[i][2][x1] - pg[i][2][x0]) -
                      lx * (double)(pg[i][4][x1] - pg[i][4][x0]) -
                      ly * (double)(pg[i][3][x1] - pg[i][3][x0]) + n * lx * ly;
                coh = (gxx + gyy > 0.0)
                          ? sqrt((gxx - gyy) * (gxx - gyy) + 4.0 * gxy * gxy) / (gxx + gyy)
                          : 1.0;
                steps = (coh - HDRG_DB_COH_LO) / (HDRG_DB_COH_HI - HDRG_DB_COH_LO);
                steps = HDRG_DB_MAX(steps, (HDRG_DB_GRAIN * db->strength - worst) /
                                               ((HDRG_DB_GRAIN - HDRG_DB_QUANT) * db->strength));
                weight *= HDRG_DB_MIN(HDRG_DB_MAX(steps, 0.0), 1.0);
                grain = weight <= 0.0;
            }
        }

        if (weight <= 0.0) {
            continue;
        }

        for (c = 0; c < 3u; c++) {
            v = (double)in[(size_t)x * 3u + c] +
                weight * (plane[c] - (double)in[(size_t)x * 3u + c]);
            out[(size_t)x * 3u + c] = (uint16_t)HDRG_DB_MIN(HDRG_DB_MAX(v + 0.5, 0.0), 65535.0);
        }
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_db_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_deband_t *db = (const hdrg_deband_t *)inst;
    const uint8_t *in;
    const size_t n1 = (size_t)db->width + 1u;
    hdrg_db_columns_t cols[HDRG_DB_RADII];
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, r, i, lo, hi;
    uint8_t *out, *flags, c, g;
    int64_t *columns, *prefix;
    int32_t *luma;
    size_t in_stride, out_stride, each, pixels, sums;
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

    /*
     * per radius fifteen column sums (3 channels x 3, edges, 5 of the Y'
     * differences), then the prefix sums (18 rows)
     */
    each = 15u * (size_t)db->width;
    pixels = (size_t)(in_end - in_begin) * db->width;
    sums = (HDRG_DB_RADII * each + HDRG_DB_RADII * 18u * n1) * sizeof(int64_t);

    /* the job's scratch: the column sums (zeroed), the Y' rows, the flags */
    status = api->job_scratch(job, sums + pixels * 3u * sizeof(int32_t) + pixels, &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    columns = (int64_t *)scratch;
    luma = (int32_t *)(void *)((uint8_t *)scratch + sums);
    flags = (uint8_t *)scratch + sums + pixels * 3u * sizeof(int32_t);
    memset(columns, 0, sums);

    prefix = columns + HDRG_DB_RADII * each;

    for (i = 0; i < HDRG_DB_RADII; i++) {
        for (c = 0; c < 3u; c++) {
            cols[i].sum[c] = columns + i * each + (3u * c) * (size_t)db->width;
            cols[i].square[c] = cols[i].sum[c] + db->width;
            cols[i].ycol[c] = cols[i].square[c] + db->width;
        }

        cols[i].edge = columns + i * each + 9u * (size_t)db->width;

        for (g = 0; g < 5u; g++) {
            cols[i].grad[g] = cols[i].edge + (1u + g) * (size_t)db->width;
        }
    }

    hdrg_db_lumas(db, in, in_stride, in_begin, in_end, luma);
    hdrg_db_edges(db, luma, in_begin, in_end, luma + pixels, luma + 2u * pixels, flags);

    /* the windows of row begin, then slid a row at a time (rows outside the frame are not read) */
    for (i = 0; i < HDRG_DB_RADII; i++) {
        r = hdrg_db_radii[i];
        lo = (begin > r) ? begin - r : 0u;
        hi = HDRG_DB_MIN(begin + r + 1u, db->height);

        for (y = lo; y < hi; y++) {
            hdrg_db_column_row(db, in, in_stride, in_begin, y, luma,
                               flags + (size_t)(y - in_begin) * db->width, 1, &cols[i]);
        }
    }

    for (y = begin; y < end; y++) {
        if (y > begin) {
            for (i = 0; i < HDRG_DB_RADII; i++) {
                r = hdrg_db_radii[i];

                if (y >= r + 1u) {
                    hdrg_db_column_row(db, in, in_stride, in_begin, y - r - 1u, luma,
                                       flags + (size_t)(y - r - 1u - in_begin) * db->width, -1,
                                       &cols[i]);
                }

                if (y + r < db->height) {
                    hdrg_db_column_row(db, in, in_stride, in_begin, y + r, luma,
                                       flags + (size_t)(y + r - in_begin) * db->width, 1, &cols[i]);
                }
            }
        }

        hdrg_db_row(db, cols, prefix, y,
                    (const uint16_t *)(const void *)(in + (size_t)(y - in_begin) * in_stride),
                    (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride));
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_deband_init(hdrg_plugin_t *plugin,
                                                              const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_DB_ID), HDRG_LIT("Debanding"),
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

    status = api->class_add_option_float(
        cls, HDRG_LIT("strength"), 1.0, 0.0, 3.0,
        HDRG_LIT("scales the residual (in 8-bit codes, 0.8 to 1.6) a fit takes (0 = off)"));
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
        status = hdrg_webgpu_deband_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_deband_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_deband_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}

extern hdrg_status_t hdrg_deband_settings(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                          double strength, bool *active)
{
    hdrg_status_t status;
    uint32_t version;
    uint8_t i;
    size_t needed;
    float noise[HDRG_SD_NOISE_FLOATS], quiet;

    *active = strength > 0.0;
    status = api->nego_side_data(nego, HDRG_SD_NOISE, &version, noise, sizeof(noise), &needed);

    /* the quietest level's noise alone leaves a residual of no weight: nothing to work on */
    if (status == HDRG_OK && version == HDRG_SD_NOISE_VERSION && needed == sizeof(noise)) {
        quiet = noise[0];

        for (i = 1; i <= 8u; i++) {
            if (noise[i] >= 0.0f && (quiet < 0.0f || noise[i] < quiet)) {
                quiet = noise[i];
            }
        }

        if (quiet >= 0.0f && (double)quiet * 65535.0 >= HDRG_DB_HI * strength) {
            *active = false;
        }
    } else if (status != HDRG_OK && status != HDRG_ERR_NOT_FOUND) {
        return status;
    }

    return HDRG_OK;
}
