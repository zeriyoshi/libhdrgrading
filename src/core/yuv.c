/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "yuv.h"

#include <string.h>

#include "../priv.h"

#define HDRG_YUV_CFL_EPS    0.00025f /* (4 / 255)^2: the luma variance a chroma slope needs */
#define HDRG_YUV_CFL_MAX    2.0f
#define HDRG_YUV_INV_CODE16 (1.0f / 65535.0f)
#define HDRG_YUV_CHUNK      128u /* chroma columns a row pass works on at once */
#define HDRG_YUV_SUMS       8u   /* a column's window sums: Y', Y'^2, then each C, Y' C, C^2 */
#define HDRG_YUV_SEG_FLOOR                                                                         \
    0.000246f /* (4 / 255)^2: the mean square residual of the line a bend needs */
#define HDRG_YUV_SEG_SPAN   0.047f /* 12 / 255: the luma range of the window a bend needs */
#define HDRG_YUV_SEG_RIDGE  0.05f  /* the pull of each knot toward the line, in samples */
#define HDRG_YUV_SEG_FULL   0.3f   /* the bend's residual over the line's: all of the bend ... */
#define HDRG_YUV_SEG_NONE   0.7f   /* ... and none of it */
#define HDRG_YUV_SEG_PIXELS 49u    /* the luma pixels of a 3 x 3 window of sites at most */
#define HDRG_YUV_SEG_SPREAD 0.25f  /* the knots' reach past the window's samples, x their range */
#define HDRG_YUV_BAND       256u   /* to R'G'B', subsampled: the luma rows a band emits ... */
#define HDRG_YUV_MARGIN     16u    /* ... with these of context on each side (the passes reach 6) */

typedef struct {
    const hdrg_yuv_t *yuv;
    const uint16_t *src; /* to Y'CbCr: the rows from y0 */
    uint16_t *rgb;       /* to R'G'B' ... */
    size_t stride;       /* ... in rows of these bytes */
    float *site;         /* to R'G'B', subsampled: the luma filtered to each chroma site, ... */
    float *slope[2];     /* ... the slope of Cb, Cr against it (chroma width x height) ... */
    float *chroma[2];    /* ... and its Cb, Cr centered ([-0.5, 0.5]) */
    float *bent;         /* SEGMENTED: 1 where the chroma follows the bent line: ... */
    float *knot[3];      /* ... its knots ... */
    float *value[2][3];  /* ... and Cb, Cr at them less the site's own (the line's slope kept) */
    uint32_t y0, rows;
    uint32_t first; /* to R'G'B': the picture's first row emitted (a band's after its margin) */
    float kr, kb, kg;
    /* R' = Y' + cr_r Cr, B' = Y' + cb_b Cb, G' = Y' - cb_g Cb - cr_g Cr */
    float cr_r, cb_b, cb_g, cr_g;
    float inv_cr_r, inv_cb_b;
} hdrg_yuv_job_t;

/* code = off + range * value (Y' in [0, 1], C in [-0.5, 0.5]); inv_*: 1 / range */
typedef struct {
    float y_off, y_range, c_off, c_range, top, inv_y, inv_c;
} hdrg_yuv_range_t;

/* the chroma at a luma pixel: its codes, the site luma and the slopes interpolated alike */
typedef struct {
    float cb, cr, site, slope[2];
} hdrg_yuv_chroma_t;

static inline uint32_t hdrg_yuv_chroma_width(const hdrg_yuv_t *yuv)
{
    return (yuv->layout == HDRG_YUV_444) ? yuv->width : (yuv->width + 1u) / 2u;
}

static inline uint32_t hdrg_yuv_chroma_height(const hdrg_yuv_t *yuv)
{
    return (yuv->layout == HDRG_YUV_420) ? (yuv->height + 1u) / 2u : yuv->height;
}

/* the luma weights (H.273) and the conversion's coefficients */
static inline void hdrg_yuv_matrix(uint8_t matrix, hdrg_yuv_job_t *job)
{
    if (matrix == HDRG_MATRIX_BT2020) {
        job->kr = 0.2627f;
        job->kb = 0.0593f;
    } else if (matrix == HDRG_MATRIX_BT601) {
        job->kr = 0.299f;
        job->kb = 0.114f;
    } else {
        job->kr = 0.2126f;
        job->kb = 0.0722f;
    }

    job->kg = 1.0f - job->kr - job->kb;
    job->cr_r = 2.0f * (1.0f - job->kr);
    job->cb_b = 2.0f * (1.0f - job->kb);
    job->cb_g = 2.0f * job->kb * (1.0f - job->kb) / job->kg;
    job->cr_g = 2.0f * job->kr * (1.0f - job->kr) / job->kg;
    job->inv_cr_r = 1.0f / job->cr_r;
    job->inv_cb_b = 1.0f / job->cb_b;
}

/*
 * H.273: video range 16..235 / 16..240 (x 2^(bits - 8)), full range 0..2^bits - 1; read
 * (samples) msb: the codes x 2^(16 - bits), which reads the samples as their codes exactly
 */
static inline void hdrg_yuv_range(const hdrg_yuv_t *yuv, bool read, hdrg_yuv_range_t *range)
{
    const float scale = (float)(1u << (yuv->bits - 8u)),
                aligned = (read && yuv->msb) ? (float)(1u << (16u - yuv->bits)) : 1.0f;

    range->top = (float)((1u << yuv->bits) - 1u);

    if (yuv->full_range) {
        range->y_off = 0.0f;
        range->y_range = range->top;
        range->c_off = yuv->jfif ? 128.0f * range->top / 255.0f : (float)(1u << (yuv->bits - 1u));
        range->c_range = range->top;
    } else {
        range->y_off = 16.0f * scale;
        range->y_range = 219.0f * scale;
        range->c_off = 128.0f * scale;
        range->c_range = 224.0f * scale;
    }

    range->y_off *= aligned;
    range->y_range *= aligned;
    range->c_off *= aligned;
    range->c_range *= aligned;
    range->top *= aligned;
    range->inv_y = 1.0f / range->y_range;
    range->inv_c = 1.0f / range->c_range;
}

/* plane's sample x of a row: its place in the row's samples (interleaved: Cb, Cr alternating) */
static inline size_t hdrg_yuv_place(const hdrg_yuv_t *yuv, uint8_t plane, uint32_t x)
{
    return (yuv->interleaved && plane != 0) ? (2u * (size_t)x) + plane - 1u : x;
}

/* the plane whose rows hold plane's samples */
static inline uint8_t hdrg_yuv_holder(const hdrg_yuv_t *yuv, uint8_t plane)
{
    return (yuv->interleaved && plane != 0) ? 1u : plane;
}

static inline float hdrg_yuv_sample(const hdrg_yuv_t *yuv, uint8_t plane, uint32_t x, uint32_t y)
{
    const uint8_t holder = hdrg_yuv_holder(yuv, plane),
                  *row = (const uint8_t *)yuv->planes[holder] + (size_t)y * yuv->strides[holder];
    const size_t at = hdrg_yuv_place(yuv, plane, x);
    uint16_t wide;

    if (yuv->bits == 8) {
        return (float)row[at];
    }

    memcpy(&wide, row + at * 2u, sizeof(wide));

    return (float)wide;
}

/* the luma of pixel (x, y) in [0, 1] */
static inline float hdrg_yuv_luma(const hdrg_yuv_t *yuv, const hdrg_yuv_range_t *range, uint32_t x,
                                  uint32_t y)
{
    return (hdrg_yuv_sample(yuv, 0, x, y) - range->y_off) * range->inv_y;
}

/*
 * Luma index i between the subsampled samples near (weight w) and far: they
 * sit d / 2 luma samples after the even ones (0: on them, 1: between two,
 * 2: on the odd ones); n of them
 */
static inline void hdrg_yuv_axis(uint32_t i, uint32_t d, uint32_t n, uint32_t *near, uint32_t *far,
                                 float *w)
{
    uint32_t q;

    if (2u * i < d) {
        *near = 0;
        *far = 0;
        *w = 1.0f;

        return;
    }

    q = 2u * i - d;
    *near = HDRG_MIN(q / 4u, n - 1u);
    *far = HDRG_MIN(*near + 1u, n - 1u);
    *w = (float)(4u - q % 4u) / 4.0f;
}

/* the offsets d of hdrg_yuv_axis: columns, rows (2 + 1: not subsampled) */
static inline void hdrg_yuv_offsets(const hdrg_yuv_t *yuv, uint32_t *dx, uint32_t *dy)
{
    *dx = (yuv->siting % 2u == 1u) ? 1u : 0u;
    *dy = (yuv->siting <= HDRG_CHROMA_CENTER) ? 1u : ((yuv->siting <= HDRG_CHROMA_TOP) ? 0u : 2u);

    if (yuv->layout == HDRG_YUV_444) {
        *dx = 3u;
    }

    if (yuv->layout != HDRG_YUV_420) {
        *dy = 3u;
    }
}

/*
 * the taps of a site on an axis: offset d of hdrg_yuv_axis at subsampled
 * index k (on a luma sample: [1 2 1] / 4 around it, between two: their mean,
 * 3: luma index k itself); n luma samples. Returns the count
 */
static inline uint8_t hdrg_yuv_taps(uint32_t d, uint32_t k, uint32_t n, uint32_t at[3], float w[3])
{
    const uint32_t c = 2u * k + d / 2u;

    if (d == 3u) {
        at[0] = k;
        w[0] = 1.0f;

        return 1;
    }

    if (d == 1u) {
        at[0] = 2u * k;
        at[1] = HDRG_MIN(2u * k + 1u, n - 1u);
        w[0] = 0.5f;
        w[1] = 0.5f;

        return 2;
    }

    at[0] = (c > 0u) ? HDRG_MIN(c - 1u, n - 1u) : 0u;
    at[1] = HDRG_MIN(c, n - 1u);
    at[2] = HDRG_MIN(c + 1u, n - 1u);
    w[0] = 0.25f;
    w[1] = 0.5f;
    w[2] = 0.25f;

    return 3;
}

/*
 * the luma filtered to chroma site (j, k) the way an encoder filters chroma
 * to it (hdrg_yuv_taps on each axis)
 */
static inline float hdrg_yuv_site_luma(const hdrg_yuv_t *yuv, const hdrg_yuv_range_t *range,
                                       uint32_t j, uint32_t k)
{
    uint32_t dx, dy, ax[3], ay[3];
    uint8_t nx, ny, a, b;
    float wx[3], wy[3], s, row;

    hdrg_yuv_offsets(yuv, &dx, &dy);
    nx = hdrg_yuv_taps(dx, j, yuv->width, ax, wx);
    ny = hdrg_yuv_taps(dy, k, yuv->height, ay, wy);
    s = 0.0f;

    for (b = 0; b < ny; b++) {
        row = 0.0f;

        for (a = 0; a < nx; a++) {
            row += wx[a] * hdrg_yuv_luma(yuv, range, ax[a], ay[b]);
        }

        s += wy[b] * row;
    }

    return s;
}

/* chroma column j between chroma rows near (weight w) and far */
static inline float hdrg_yuv_vertical(const hdrg_yuv_t *yuv, uint8_t plane, uint32_t j,
                                      uint32_t near, uint32_t far, float w)
{
    if (near == far) {
        return hdrg_yuv_sample(yuv, plane, j, near);
    }

    return w * hdrg_yuv_sample(yuv, plane, j, near) +
           (1.0f - w) * hdrg_yuv_sample(yuv, plane, j, far);
}

/* column j of a chroma-site plane between chroma rows near (weight w) and far */
static inline float hdrg_yuv_plane_vertical(const float *plane, uint32_t cw, uint32_t j,
                                            uint32_t near, uint32_t far, float w)
{
    if (near == far) {
        return plane[(size_t)near * cw + j];
    }

    return w * plane[(size_t)near * cw + j] + (1.0f - w) * plane[(size_t)far * cw + j];
}

static inline uint16_t hdrg_yuv_code16(float v)
{
    return (uint16_t)(HDRG_CLAMP(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

/* the weights of luma l on the hats of knots k (beyond the ends: the end's) */
static inline void hdrg_yuv_hats(const float k[3], float l, float h[3])
{
    float t;

    h[0] = 0.0f;
    h[1] = 0.0f;
    h[2] = 0.0f;

    if (l <= k[0]) {
        h[0] = 1.0f;

        return;
    }

    if (l >= k[2]) {
        h[2] = 1.0f;

        return;
    }

    if (l < k[1]) {
        t = (l - k[0]) / (k[1] - k[0]);
        h[0] = 1.0f - t;
        h[1] = t;

        return;
    }

    t = (l - k[1]) / (k[2] - k[1]);
    h[1] = 1.0f - t;
    h[2] = t;
}

/* the middle of the 3-means of n values in [lo, hi] (Lloyd from the ends and the midpoint) */
static inline float hdrg_yuv_middle(const float *v, uint32_t n, float lo, float hi)
{
    uint32_t i, round;
    uint8_t c, best;
    float center[3], sum[3], count[3], d, nearest;

    center[0] = lo;
    center[1] = 0.5f * (lo + hi);
    center[2] = hi;

    for (round = 0; round < 4u; round++) {
        for (c = 0; c < 3u; c++) {
            sum[c] = 0.0f;
            count[c] = 0.0f;
        }

        for (i = 0; i < n; i++) {
            best = 0;
            nearest = (v[i] - center[0]) * (v[i] - center[0]);

            for (c = 1; c < 3u; c++) {
                d = (v[i] - center[c]) * (v[i] - center[c]);

                if (d < nearest) {
                    best = c;
                    nearest = d;
                }
            }

            sum[best] += v[i];
            count[best] += 1.0f;
        }

        for (c = 0; c < 3u; c++) {
            if (count[c] > 0.0f) {
                center[c] = sum[c] / count[c];
            }
        }
    }

    return center[1];
}

/* x of a x = b by Cramer's rule (false: singular) */
static inline bool hdrg_yuv_solve3(const double a[3][3], const double b[3], double x[3])
{
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                       a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);

    if (!(det > 1e-12 || det < -1e-12)) {
        return false;
    }

    x[0] = (b[0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
            a[0][1] * (b[1] * a[2][2] - a[1][2] * b[2]) +
            a[0][2] * (b[1] * a[2][1] - a[1][1] * b[2])) /
           det;
    x[1] = (a[0][0] * (b[1] * a[2][2] - a[1][2] * b[2]) -
            b[0] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
            a[0][2] * (a[1][0] * b[2] - b[1] * a[2][0])) /
           det;
    x[2] =
        (a[0][0] * (a[1][1] * b[2] - b[1] * a[2][1]) - a[0][1] * (a[1][0] * b[2] - b[1] * a[2][0]) +
         b[0] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) /
        det;

    return true;
}

/* R'G'B' of luma pixel x (luma in [0, 1], centered chroma u, v) into its row out */
static inline void hdrg_yuv_put(const hdrg_yuv_job_t *job, uint32_t x, float luma, float u, float v,
                                uint16_t *out)
{
    out[(size_t)x * 3u] = hdrg_yuv_code16(luma + job->cr_r * v);
    out[(size_t)x * 3u + 1u] = hdrg_yuv_code16(luma - job->cb_g * u - job->cr_g * v);
    out[(size_t)x * 3u + 2u] = hdrg_yuv_code16(luma + job->cb_b * u);
}

/*
 * What the bent lines of the sites around pixel (x, y) (columns left /
 * right, wx on left; rows near / far, w on near) add to the interpolated
 * line, in centered chroma units: each at the pixel's luma or along the
 * chord between its 3 x 3 neighborhood's darkest and brightest luma,
 * whichever departs less from the line (a pixel between an outline and one
 * fill does not take a third fill of its luma; a fill at a junction keeps
 * its own)
 */
static inline void hdrg_yuv_bend(const hdrg_yuv_job_t *job, const hdrg_yuv_range_t *range,
                                 uint32_t cw, uint32_t x, uint32_t y, uint32_t left, uint32_t right,
                                 float wx, uint32_t near, uint32_t far, float w, float out[2])
{
    const hdrg_yuv_t *yuv = job->yuv;
    const uint32_t cols[2] = {left, right}, rows[2] = {near, far};
    const uint8_t nc = (right != left) ? 2u : 1u, nr = (far != near) ? 2u : 1u;
    const float col_w[2] = {(nc == 2u) ? wx : 1.0f, 1.0f - wx},
                row_w[2] = {(nr == 2u) ? w : 1.0f, 1.0f - w};
    uint32_t xx, yy;
    uint8_t a, b, p;
    size_t at;
    bool chord;
    float k[3], h[3], lo_h[3], hi_h[3], at_l[2], on_chord[2], l, lo, hi, t, weight, f_lo, f_hi,
        line;

    out[0] = 0.0f;
    out[1] = 0.0f;
    chord = false;
    l = 0.0f;
    lo = 0.0f;
    hi = 0.0f;
    t = 0.0f;

    for (b = 0; b < nr; b++) {
        for (a = 0; a < nc; a++) {
            at = (size_t)rows[b] * cw + cols[a];

            if (job->bent[at] == 0.0f) {
                continue;
            }

            if (!chord) {
                l = hdrg_yuv_sample(yuv, 0, x, y);
                lo = l;
                hi = l;

                for (yy = (y > 0u) ? y - 1u : 0u; yy <= HDRG_MIN(y + 1u, yuv->height - 1u); yy++) {
                    for (xx = (x > 0u) ? x - 1u : 0u; xx <= HDRG_MIN(x + 1u, yuv->width - 1u);
                         xx++) {
                        f_lo = hdrg_yuv_sample(yuv, 0, xx, yy);
                        lo = HDRG_MIN(lo, f_lo);
                        hi = HDRG_MAX(hi, f_lo);
                    }
                }

                l = (l - range->y_off) * range->inv_y;
                lo = (lo - range->y_off) * range->inv_y;
                hi = (hi - range->y_off) * range->inv_y;
                t = (hi > lo) ? (l - lo) / (hi - lo) : 0.0f;
                chord = true;
            }

            k[0] = job->knot[0][at];
            k[1] = job->knot[1][at];
            k[2] = job->knot[2][at];
            hdrg_yuv_hats(k, l, h);
            hdrg_yuv_hats(k, lo, lo_h);
            hdrg_yuv_hats(k, hi, hi_h);
            weight = row_w[b] * col_w[a];

            for (p = 0; p < 2u; p++) {
                line = job->slope[p][at] * (l - job->site[at]);
                at_l[p] = h[0] * job->value[p][0][at] + h[1] * job->value[p][1][at] +
                          h[2] * job->value[p][2][at] - line;
                f_lo = lo_h[0] * job->value[p][0][at] + lo_h[1] * job->value[p][1][at] +
                       lo_h[2] * job->value[p][2][at];
                f_hi = hi_h[0] * job->value[p][0][at] + hi_h[1] * job->value[p][1][at] +
                       hi_h[2] * job->value[p][2][at];
                on_chord[p] = f_lo + t * (f_hi - f_lo) - line;
            }

            if (on_chord[0] * on_chord[0] + on_chord[1] * on_chord[1] <
                at_l[0] * at_l[0] + at_l[1] * at_l[1]) {
                at_l[0] = on_chord[0];
                at_l[1] = on_chord[1];
            }

            out[0] += weight * at_l[0];
            out[1] += weight * at_l[1];
        }
    }
}

/* pixel x of luma luma, the interpolated chroma c plus the luma's detail where it follows it */
static inline void hdrg_yuv_put_cfl(const hdrg_yuv_job_t *job, uint32_t x, float luma,
                                    const hdrg_yuv_chroma_t *c, uint16_t *out)
{
    const float d = luma - c->site;

    hdrg_yuv_put(job, x, luma, c->cb + c->slope[0] * d, c->cr + c->slope[1] * d, out);
}

/* chroma column j of the current row pair (near, far, w), centered */
static inline void hdrg_yuv_chroma_at(const hdrg_yuv_job_t *job, const hdrg_yuv_range_t *range,
                                      uint32_t cw, uint32_t j, uint32_t near, uint32_t far, float w,
                                      hdrg_yuv_chroma_t *c)
{
    c->site = 0.0f;
    c->slope[0] = 0.0f;
    c->slope[1] = 0.0f;

    if (job->site == NULL) {
        c->cb = (hdrg_yuv_vertical(job->yuv, 1, j, near, far, w) - range->c_off) * range->inv_c;
        c->cr = (hdrg_yuv_vertical(job->yuv, 2, j, near, far, w) - range->c_off) * range->inv_c;
    } else {
        c->cb = hdrg_yuv_plane_vertical(job->chroma[0], cw, j, near, far, w);
        c->cr = hdrg_yuv_plane_vertical(job->chroma[1], cw, j, near, far, w);
        c->site = hdrg_yuv_plane_vertical(job->site, cw, j, near, far, w);
        c->slope[0] = hdrg_yuv_plane_vertical(job->slope[0], cw, j, near, far, w);
        c->slope[1] = hdrg_yuv_plane_vertical(job->slope[1], cw, j, near, far, w);
    }
}

/* w a + (1 - w) b */
static inline void hdrg_yuv_chroma_mix(const hdrg_yuv_chroma_t *a, const hdrg_yuv_chroma_t *b,
                                       float w, hdrg_yuv_chroma_t *out)
{
    out->cb = w * a->cb + (1.0f - w) * b->cb;
    out->cr = w * a->cr + (1.0f - w) * b->cr;
    out->site = w * a->site + (1.0f - w) * b->site;
    out->slope[0] = w * a->slope[0] + (1.0f - w) * b->slope[0];
    out->slope[1] = w * a->slope[1] + (1.0f - w) * b->slope[1];
}

/* Cb, Cr (centered, [-0.5, 0.5]) of the R'G'B' at column x of a row */
static inline void hdrg_yuv_cbcr(const hdrg_yuv_job_t *job, const uint16_t *row, uint32_t x,
                                 float *cb, float *cr)
{
    const float r = (float)row[(size_t)x * 3u] * HDRG_YUV_INV_CODE16,
                g = (float)row[(size_t)x * 3u + 1u] * HDRG_YUV_INV_CODE16,
                b = (float)row[(size_t)x * 3u + 2u] * HDRG_YUV_INV_CODE16,
                luma = job->kr * r + job->kg * g + job->kb * b;

    *cb = (b - luma) * job->inv_cb_b;
    *cr = (r - luma) * job->inv_cr_r;
}

/*
 * count values of plane row y from column x0 (codes, clamped and rounded; msb: shifted to the
 * samples' high bits)
 */
static inline void hdrg_yuv_store_run(const hdrg_yuv_t *yuv, const hdrg_yuv_range_t *range,
                                      uint8_t plane, uint32_t x0, uint32_t y, uint32_t count,
                                      const float *v)
{
    const uint8_t holder = hdrg_yuv_holder(yuv, plane),
                  shift = yuv->msb ? (uint8_t)(16u - yuv->bits) : 0u;
    uint32_t i;
    uint16_t code;
    uint8_t *row;

    row = (uint8_t *)(uintptr_t)yuv->planes[holder] + (size_t)y * yuv->strides[holder];

    if (yuv->bits == 8) {
        for (i = 0; i < count; i++) {
            row[hdrg_yuv_place(yuv, plane, x0 + i)] =
                (uint8_t)(HDRG_CLAMP(v[i], 0.0f, range->top) + 0.5f);
        }

        return;
    }

    for (i = 0; i < count; i++) {
        code = (uint16_t)((uint32_t)(HDRG_CLAMP(v[i], 0.0f, range->top) + 0.5f) << shift);
        memcpy(row + hdrg_yuv_place(yuv, plane, x0 + i) * 2u, &code, sizeof(code));
    }
}

/*
 * Row y of R'G'B' from subsampled chroma with the luma's detail (no bend), in
 * runs of 2 HDRG_YUV_CHUNK pixels: each chroma column's Cb, Cr, site luma and
 * slopes interpolated vertically (rows near, far, w on near) once, the
 * run's luma read once, then each pixel between its two columns
 * (hdrg_yuv_axis)
 */
static void hdrg_yuv_cfl_row(const hdrg_yuv_job_t *job, const hdrg_yuv_range_t *range, uint32_t y,
                             uint32_t dx, uint32_t near, uint32_t far, float w, uint16_t *out)
{
    const hdrg_yuv_t *yuv = job->yuv;
    const uint32_t cw = hdrg_yuv_chroma_width(yuv);
    const uint8_t *row = (const uint8_t *)yuv->planes[0] + (size_t)y * yuv->strides[0];
    const float *planes[5] = {job->chroma[0], job->chroma[1], job->site, job->slope[0],
                              job->slope[1]};
    uint32_t x0, x1, x, first, last, left, right, j, a, b;
    uint16_t wide;
    uint8_t i;
    float wx, d, luma, cols[5][HDRG_YUV_CHUNK + 2u], lumas[2u * HDRG_YUV_CHUNK];

    for (x0 = 0; x0 < yuv->width; x0 += 2u * HDRG_YUV_CHUNK) {
        x1 = HDRG_MIN(x0 + 2u * HDRG_YUV_CHUNK, yuv->width);
        hdrg_yuv_axis(x0, dx, cw, &first, &right, &wx);
        hdrg_yuv_axis(x1 - 1u, dx, cw, &left, &last, &wx);

        for (i = 0; i < 5u; i++) {
            for (j = first; j <= last; j++) {
                cols[i][j - first] = hdrg_yuv_plane_vertical(planes[i], cw, j, near, far, w);
            }
        }

        if (yuv->bits == 8) {
            for (x = x0; x < x1; x++) {
                lumas[x - x0] = ((float)row[x] - range->y_off) * range->inv_y;
            }
        } else {
            for (x = x0; x < x1; x++) {
                memcpy(&wide, row + (size_t)x * 2u, sizeof(wide));
                lumas[x - x0] = ((float)wide - range->y_off) * range->inv_y;
            }
        }

        for (x = x0; x < x1; x++) {
            hdrg_yuv_axis(x, dx, cw, &left, &right, &wx);
            a = left - first;
            b = right - first;
            luma = lumas[x - x0];
            d = luma - (wx * cols[2][a] + (1.0f - wx) * cols[2][b]);
            hdrg_yuv_put(job, x, luma,
                         wx * cols[0][a] + (1.0f - wx) * cols[0][b] +
                             (wx * cols[3][a] + (1.0f - wx) * cols[3][b]) * d,
                         wx * cols[1][a] + (1.0f - wx) * cols[1][b] +
                             (wx * cols[4][a] + (1.0f - wx) * cols[4][b]) * d,
                         out);
        }
    }
}

/* chroma row k: the luma filtered to each chroma site */
static void hdrg_yuv_site_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_yuv_job_t *job = (const hdrg_yuv_job_t *)arg;
    const uint32_t cw = hdrg_yuv_chroma_width(job->yuv);
    hdrg_yuv_range_t range;
    uint32_t k, j;

    hdrg_yuv_range(job->yuv, true, &range);

    for (k = row_begin; k < row_end; k++) {
        for (j = 0; j < cw; j++) {
            job->site[(size_t)k * cw + j] = hdrg_yuv_site_luma(job->yuv, &range, j, k);
            job->chroma[0][(size_t)k * cw + j] =
                (hdrg_yuv_sample(job->yuv, 1, j, k) - range.c_off) * range.inv_c;
            job->chroma[1][(size_t)k * cw + j] =
                (hdrg_yuv_sample(job->yuv, 2, j, k) - range.c_off) * range.inv_c;
        }
    }
}

/*
 * Chroma site (j, k) of the segmented fit (the line through mean, means[]
 * with the slopes in place): where the line leaves a residual, the chroma
 * at the knots (the window's darkest, middle and brightest luma pixel)
 * fitted to the window's samples through their sites' taps, toward the
 * line by HDRG_YUV_SEG_RIDGE and within the samples' range widened by
 * HDRG_YUV_SEG_SPREAD of it (texture does not bend a fill into another
 * hue); as much of it as the residual it removes says, kept less the bent
 * line's own mean over the site (flat beyond the end knots)
 */
static void hdrg_yuv_segment(const hdrg_yuv_job_t *job, const hdrg_yuv_range_t *range, uint32_t j,
                             uint32_t k, float mean, const float means[2])
{
    const hdrg_yuv_t *yuv = job->yuv;
    const uint32_t cw = hdrg_yuv_chroma_width(yuv), ch = hdrg_yuv_chroma_height(yuv),
                   j0 = (j > 0u) ? j - 1u : 0u, j1 = HDRG_MIN(j + 1u, cw - 1u),
                   k0 = (k > 0u) ? k - 1u : 0u, k1 = HDRG_MIN(k + 1u, ch - 1u);
    const size_t at = (size_t)k * cw + j;
    uint32_t dx, dy, ax[3], ay[3], x0, y0, wide, tall, x, y, jj, kk;
    uint8_t nx, ny, a, b, n, s, p, r, c;
    double normal[3][3], rhs[3], solved[3];
    float pixels[HDRG_YUV_SEG_PIXELS], knots[3], hat[9][3], chroma[9][2], line[2][3], fitted[2][3],
        own[3], h[3], wx[3], wy[3], lo, hi, l, e, residual[2], bent, beta, beta_p, site, c_lo, c_hi;

    job->bent[at] = 0.0f;
    hdrg_yuv_offsets(yuv, &dx, &dy);

    n = 0;
    residual[0] = 0.0f;
    residual[1] = 0.0f;

    for (kk = k0; kk <= k1; kk++) {
        for (jj = j0; jj <= j1; jj++) {
            l = job->site[(size_t)kk * cw + jj];

            for (p = 0; p < 2u; p++) {
                chroma[n][p] = job->chroma[p][(size_t)kk * cw + jj];
                e = chroma[n][p] - (means[p] + job->slope[p][at] * (l - mean));
                residual[p] += e * e;
            }

            n++;
        }
    }

    if (residual[0] <= (float)n * HDRG_YUV_SEG_FLOOR &&
        residual[1] <= (float)n * HDRG_YUV_SEG_FLOOR) {
        return;
    }

    /* the luma pixels the window's taps read */
    nx = hdrg_yuv_taps(dx, j0, yuv->width, ax, wx);
    x0 = ax[0];
    ny = hdrg_yuv_taps(dy, k0, yuv->height, ay, wy);
    y0 = ay[0];
    nx = hdrg_yuv_taps(dx, j1, yuv->width, ax, wx);
    wide = ax[nx - 1u] - x0 + 1u;
    ny = hdrg_yuv_taps(dy, k1, yuv->height, ay, wy);
    tall = ay[ny - 1u] - y0 + 1u;
    lo = 1.0f;
    hi = 0.0f;

    for (y = 0; y < tall; y++) {
        for (x = 0; x < wide; x++) {
            l = hdrg_yuv_luma(yuv, range, x0 + x, y0 + y);
            pixels[y * wide + x] = l;
            lo = HDRG_MIN(lo, l);
            hi = HDRG_MAX(hi, l);
        }
    }

    if (hi - lo < HDRG_YUV_SEG_SPAN) {
        return;
    }

    knots[0] = lo;
    knots[1] = HDRG_CLAMP(hdrg_yuv_middle(pixels, wide * tall, lo, hi), lo + 0.1f * (hi - lo),
                          hi - 0.1f * (hi - lo));
    knots[2] = hi;

    /* each sample's hats through its taps, the normal equations */
    for (r = 0; r < 3u; r++) {
        for (c = 0; c < 3u; c++) {
            normal[r][c] = (r == c) ? (double)HDRG_YUV_SEG_RIDGE : 0.0;
        }
    }

    s = 0;

    for (kk = k0; kk <= k1; kk++) {
        ny = hdrg_yuv_taps(dy, kk, yuv->height, ay, wy);

        for (jj = j0; jj <= j1; jj++) {
            nx = hdrg_yuv_taps(dx, jj, yuv->width, ax, wx);
            hat[s][0] = 0.0f;
            hat[s][1] = 0.0f;
            hat[s][2] = 0.0f;

            for (b = 0; b < ny; b++) {
                for (a = 0; a < nx; a++) {
                    hdrg_yuv_hats(knots, pixels[(ay[b] - y0) * wide + ax[a] - x0], h);

                    for (c = 0; c < 3u; c++) {
                        hat[s][c] += wy[b] * wx[a] * h[c];
                    }
                }
            }

            for (r = 0; r < 3u; r++) {
                for (c = 0; c < 3u; c++) {
                    normal[r][c] += (double)hat[s][r] * (double)hat[s][c];
                }
            }

            s++;
        }
    }

    beta = 0.0f;

    for (p = 0; p < 2u; p++) {
        for (r = 0; r < 3u; r++) {
            line[p][r] = means[p] + job->slope[p][at] * (knots[r] - mean);
            fitted[p][r] = line[p][r];
            rhs[r] = (double)HDRG_YUV_SEG_RIDGE * (double)line[p][r];

            for (s = 0; s < n; s++) {
                rhs[r] += (double)hat[s][r] * (double)chroma[s][p];
            }
        }

        if (!hdrg_yuv_solve3((const double (*)[3])normal, rhs, solved)) {
            continue;
        }

        residual[p] = 0.0f;
        bent = 0.0f;

        for (s = 0; s < n; s++) {
            e = chroma[s][p] -
                (hat[s][0] * line[p][0] + hat[s][1] * line[p][1] + hat[s][2] * line[p][2]);
            residual[p] += e * e;
            e = chroma[s][p] - (hat[s][0] * (float)solved[0] + hat[s][1] * (float)solved[1] +
                                hat[s][2] * (float)solved[2]);
            bent += e * e;
        }

        if (residual[p] <= (float)n * HDRG_YUV_SEG_FLOOR) {
            continue;
        }

        beta_p = HDRG_CLAMP((HDRG_YUV_SEG_NONE - bent / residual[p]) /
                                (HDRG_YUV_SEG_NONE - HDRG_YUV_SEG_FULL),
                            0.0f, 1.0f);

        c_lo = chroma[0][p];
        c_hi = chroma[0][p];

        for (s = 1; s < n; s++) {
            c_lo = HDRG_MIN(c_lo, chroma[s][p]);
            c_hi = HDRG_MAX(c_hi, chroma[s][p]);
        }

        e = HDRG_YUV_SEG_SPREAD * (c_hi - c_lo);

        for (r = 0; r < 3u; r++) {
            fitted[p][r] = HDRG_CLAMP(line[p][r] + beta_p * ((float)solved[r] - line[p][r]),
                                      c_lo - e, c_hi + e);
        }

        beta = HDRG_MAX(beta, beta_p);
    }

    if (beta <= 0.0f) {
        return;
    }

    /* less the bent line's own mean over the site's taps */
    nx = hdrg_yuv_taps(dx, j, yuv->width, ax, wx);
    ny = hdrg_yuv_taps(dy, k, yuv->height, ay, wy);
    own[0] = 0.0f;
    own[1] = 0.0f;
    own[2] = 0.0f;

    for (b = 0; b < ny; b++) {
        for (a = 0; a < nx; a++) {
            hdrg_yuv_hats(knots, pixels[(ay[b] - y0) * wide + ax[a] - x0], h);

            for (r = 0; r < 3u; r++) {
                own[r] += wy[b] * wx[a] * h[r];
            }
        }
    }

    for (p = 0; p < 2u; p++) {
        site = own[0] * fitted[p][0] + own[1] * fitted[p][1] + own[2] * fitted[p][2];

        for (r = 0; r < 3u; r++) {
            job->value[p][r][at] = fitted[p][r] - site;
        }
    }

    job->knot[0][at] = knots[0];
    job->knot[1][at] = knots[1];
    job->knot[2][at] = knots[2];
    job->bent[at] = 1.0f;
}

/* chroma row k: the slopes of Cb and Cr against the site luma over each 3 x 3 neighborhood */
/*
 * The slopes of each chroma site from its 3 x 3 window of sites (clamped at
 * the edges): per run of HDRG_YUV_CHUNK sites, each column's sums down the
 * window's rows, then across its columns
 */
static void hdrg_yuv_slope_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_yuv_job_t *job = (const hdrg_yuv_job_t *)arg;
    const hdrg_yuv_t *yuv = job->yuv;
    const uint32_t cw = hdrg_yuv_chroma_width(yuv), ch = hdrg_yuv_chroma_height(yuv);
    hdrg_yuv_range_t range;
    uint32_t k, j, j0, j1, c0, c1, kk, k0, k1, jj, lo, hi;
    uint8_t p, i;
    size_t at;
    float rows, n, l, c, mean, var, slope, cov, vc, means[2], s[HDRG_YUV_SUMS],
        col[HDRG_YUV_CHUNK + 2u][HDRG_YUV_SUMS];

    hdrg_yuv_range(yuv, true, &range);

    for (k = row_begin; k < row_end; k++) {
        k0 = (k > 0u) ? k - 1u : 0u;
        k1 = HDRG_MIN(k + 1u, ch - 1u);
        rows = (float)(k1 - k0 + 1u);

        for (j0 = 0; j0 < cw; j0 += HDRG_YUV_CHUNK) {
            j1 = HDRG_MIN(j0 + HDRG_YUV_CHUNK, cw);
            c0 = (j0 > 0u) ? j0 - 1u : 0u;
            c1 = HDRG_MIN(j1, cw - 1u);

            for (jj = c0; jj <= c1; jj++) {
                memset(col[jj - c0], 0, sizeof(col[0]));

                for (kk = k0; kk <= k1; kk++) {
                    at = (size_t)kk * cw + jj;
                    l = job->site[at];
                    col[jj - c0][0] += l;
                    col[jj - c0][1] += l * l;

                    for (p = 0; p < 2u; p++) {
                        c = job->chroma[p][at];
                        col[jj - c0][2u + p] += c;
                        col[jj - c0][4u + p] += l * c;
                        col[jj - c0][6u + p] += c * c;
                    }
                }
            }

            for (j = j0; j < j1; j++) {
                lo = (j > 0u) ? j - 1u : 0u;
                hi = HDRG_MIN(j + 1u, cw - 1u);
                n = rows * (float)(hi - lo + 1u);

                for (i = 0; i < HDRG_YUV_SUMS; i++) {
                    s[i] = 0.0f;

                    for (jj = lo; jj <= hi; jj++) {
                        s[i] += col[jj - c0][i];
                    }
                }

                mean = s[0] / n;
                var = HDRG_MAX(s[1] / n - mean * mean, 0.0f);

                for (p = 0; p < 2u; p++) {
                    cov = s[4u + p] / n - mean * (s[2u + p] / n);
                    slope = cov / (var + HDRG_YUV_CFL_EPS);

                    /* the line: times the correlation's square (noise and texture do not follow) */
                    if (yuv->fit == HDRG_CHROMA_FIT_LINEAR) {
                        vc = HDRG_MAX(s[6u + p] / n - (s[2u + p] / n) * (s[2u + p] / n), 0.0f);
                        slope *= (var > 0.0f && vc > 0.0f) ? cov * cov / (var * vc) : 0.0f;
                    }

                    job->slope[p][(size_t)k * cw + j] =
                        HDRG_CLAMP(slope, -HDRG_YUV_CFL_MAX, HDRG_YUV_CFL_MAX);
                    means[p] = s[2u + p] / n;
                }

                if (job->bent != NULL) {
                    hdrg_yuv_segment(job, &range, j, k, mean, means);
                }
            }
        }
    }
}

/*
 * Rows of R'G'B': the chroma between the two nearest sites on each axis
 * (hdrg_yuv_axis), each chroma column interpolated vertically once per row
 * and kept for the next pixels; subsampled chroma takes the luma's detail by
 * the interpolated slopes
 */
static void hdrg_yuv_rgb_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_yuv_job_t *job = (const hdrg_yuv_job_t *)arg;
    const hdrg_yuv_t *yuv = job->yuv;
    const uint32_t cw = hdrg_yuv_chroma_width(yuv), ch = hdrg_yuv_chroma_height(yuv);
    hdrg_yuv_range_t range;
    hdrg_yuv_chroma_t lo, hi, mid;
    uint32_t y, x, dx, dy, near, far, left, right, lo_j, hi_j;
    uint16_t *out;
    float w, wx, luma, bend[2];

    hdrg_yuv_range(yuv, true, &range);
    hdrg_yuv_offsets(yuv, &dx, &dy);
    memset(&lo, 0, sizeof(lo));
    memset(&hi, 0, sizeof(hi));

    for (y = job->first + row_begin; y < job->first + row_end; y++) {
        out = (uint16_t *)(void *)((uint8_t *)job->rgb + (size_t)y * job->stride);
        near = y;
        far = y;
        w = 1.0f;
        lo_j = UINT32_MAX;
        hi_j = UINT32_MAX;

        if (dy != 3u) {
            hdrg_yuv_axis(y, dy, ch, &near, &far, &w);
        }

        if (job->bent == NULL && job->site != NULL && dx != 3u) {
            hdrg_yuv_cfl_row(job, &range, y, dx, near, far, w, out);

            continue;
        }

        for (x = 0; x < yuv->width; x++) {
            left = x;
            right = x;
            wx = 1.0f;

            if (dx != 3u) {
                hdrg_yuv_axis(x, dx, cw, &left, &right, &wx);
            }

            if (left != lo_j) {
                if (left == hi_j) {
                    lo = hi;
                } else {
                    hdrg_yuv_chroma_at(job, &range, cw, left, near, far, w, &lo);
                }

                lo_j = left;
            }

            if (right != lo_j && right != hi_j) {
                hdrg_yuv_chroma_at(job, &range, cw, right, near, far, w, &hi);
                hi_j = right;
            }

            mid = lo;

            if (right != lo_j) {
                hdrg_yuv_chroma_mix(&lo, &hi, wx, &mid);
            }

            luma = hdrg_yuv_luma(yuv, &range, x, y);

            if (dx == 3u) {
                hdrg_yuv_put(job, x, luma, mid.cb, mid.cr, out);

                continue;
            }

            if (job->bent != NULL) {
                hdrg_yuv_bend(job, &range, cw, x, y, left, right, wx, near, far, w, bend);
                mid.cb += bend[0];
                mid.cr += bend[1];
            }

            hdrg_yuv_put_cfl(job, x, luma, &mid, out);
        }
    }
}

/*
 * One job row = a chroma row: luma rows 2k and 2k + 1 of the band, in runs of
 * 2 HDRG_YUV_CHUNK pixels; each chroma sample [1 2 1] / 4 across the pixels
 * around its left column (the edge repeated), the two rows averaged
 */
static void hdrg_yuv_420_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_yuv_job_t *job = (const hdrg_yuv_job_t *)arg;
    const hdrg_yuv_t *yuv = job->yuv;
    const uint16_t *rows[2], *row;
    const size_t span = (size_t)yuv->width * 3u;
    hdrg_yuv_range_t range;
    uint32_t k, n, x, x0, x1, p0, p1, j, j0, j1, l, r;
    uint8_t i;
    float cb_sum, cr_sum, values[2u * HDRG_YUV_CHUNK], cbs[2][2u * HDRG_YUV_CHUNK + 2u],
        crs[2][2u * HDRG_YUV_CHUNK + 2u], out_cb[HDRG_YUV_CHUNK], out_cr[HDRG_YUV_CHUNK];

    hdrg_yuv_range(yuv, false, &range);

    for (k = row_begin; k < row_end; k++) {
        rows[0] = job->src + (size_t)(2u * k) * span;
        rows[1] = (2u * k + 1u < job->rows) ? rows[0] + span : rows[0];

        for (x0 = 0; x0 < yuv->width; x0 += 2u * HDRG_YUV_CHUNK) {
            x1 = HDRG_MIN(x0 + 2u * HDRG_YUV_CHUNK, yuv->width);

            for (n = 0; n < 2u && 2u * k + n < job->rows; n++) {
                row = rows[n];

                for (x = x0; x < x1; x++) {
                    values[x - x0] =
                        range.y_off +
                        range.y_range *
                            (job->kr * (float)row[(size_t)x * 3u] * HDRG_YUV_INV_CODE16 +
                             job->kg * (float)row[(size_t)x * 3u + 1u] * HDRG_YUV_INV_CODE16 +
                             job->kb * (float)row[(size_t)x * 3u + 2u] * HDRG_YUV_INV_CODE16);
                }

                hdrg_yuv_store_run(yuv, &range, 0, x0, job->y0 + 2u * k + n, x1 - x0, values);
            }

            p0 = (x0 > 0u) ? x0 - 1u : 0u;
            p1 = HDRG_MIN(x1, yuv->width - 1u);

            for (i = 0; i < 2u; i++) {
                for (x = p0; x <= p1; x++) {
                    hdrg_yuv_cbcr(job, rows[i], x, &cbs[i][x - p0], &crs[i][x - p0]);
                }
            }

            j0 = x0 / 2u;
            j1 = (x1 + 1u) / 2u;

            for (j = j0; j < j1; j++) {
                x = 2u * j;
                l = ((x > 0u) ? x - 1u : 0u) - p0;
                r = HDRG_MIN(x + 1u, yuv->width - 1u) - p0;
                cb_sum = 0.0f;
                cr_sum = 0.0f;

                for (i = 0; i < 2u; i++) {
                    cb_sum += 0.25f * cbs[i][l] + 0.5f * cbs[i][x - p0] + 0.25f * cbs[i][r];
                    cr_sum += 0.25f * crs[i][l] + 0.5f * crs[i][x - p0] + 0.25f * crs[i][r];
                }

                out_cb[j - j0] = range.c_off + range.c_range * 0.5f * cb_sum;
                out_cr[j - j0] = range.c_off + range.c_range * 0.5f * cr_sum;
            }

            hdrg_yuv_store_run(yuv, &range, 1, j0, (job->y0 / 2u) + k, j1 - j0, out_cb);
            hdrg_yuv_store_run(yuv, &range, 2, j0, (job->y0 / 2u) + k, j1 - j0, out_cr);
        }
    }
}

static void hdrg_yuv_444_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_yuv_job_t *job = (const hdrg_yuv_job_t *)arg;
    const hdrg_yuv_t *yuv = job->yuv;
    const uint16_t *row;
    const size_t span = (size_t)yuv->width * 3u;
    hdrg_yuv_range_t range;
    uint32_t k, x, x0, x1;
    float cb, cr, values[3][2u * HDRG_YUV_CHUNK];

    hdrg_yuv_range(yuv, false, &range);

    for (k = row_begin; k < row_end; k++) {
        row = job->src + (size_t)k * span;

        for (x0 = 0; x0 < yuv->width; x0 += 2u * HDRG_YUV_CHUNK) {
            x1 = HDRG_MIN(x0 + 2u * HDRG_YUV_CHUNK, yuv->width);

            for (x = x0; x < x1; x++) {
                values[0][x - x0] =
                    range.y_off +
                    range.y_range *
                        (job->kr * (float)row[(size_t)x * 3u] * HDRG_YUV_INV_CODE16 +
                         job->kg * (float)row[(size_t)x * 3u + 1u] * HDRG_YUV_INV_CODE16 +
                         job->kb * (float)row[(size_t)x * 3u + 2u] * HDRG_YUV_INV_CODE16);
                hdrg_yuv_cbcr(job, row, x, &cb, &cr);
                values[1][x - x0] = range.c_off + range.c_range * cb;
                values[2][x - x0] = range.c_off + range.c_range * cr;
            }

            hdrg_yuv_store_run(yuv, &range, 0, x0, job->y0 + k, x1 - x0, values[0]);
            hdrg_yuv_store_run(yuv, &range, 1, x0, job->y0 + k, x1 - x0, values[1]);
            hdrg_yuv_store_run(yuv, &range, 2, x0, job->y0 + k, x1 - x0, values[2]);
        }
    }
}

/* rows [first, first + rows) of a picture's conversion to R'G'B' (rgb: its first row's) */
static void hdrg_yuv_band_to_rgb16(const hdrg_exec_t *exec, const hdrg_yuv_t *yuv, float *scratch,
                                   uint16_t *rgb, size_t stride, uint32_t first, uint32_t rows)
{
    const size_t sites = (size_t)hdrg_yuv_chroma_width(yuv) * hdrg_yuv_chroma_height(yuv);
    hdrg_yuv_job_t job;
    uint8_t p;

    memset(&job, 0, sizeof(job));
    job.yuv = yuv;
    job.rgb = rgb;
    job.stride = stride;
    job.first = first;
    hdrg_yuv_matrix(yuv->matrix, &job);

    if (yuv->layout != HDRG_YUV_444 && scratch != NULL) {
        job.site = scratch;
        job.slope[0] = scratch + sites;
        job.slope[1] = scratch + 2u * sites;
        job.chroma[0] = scratch + 3u * sites;
        job.chroma[1] = scratch + 4u * sites;

        if (yuv->fit == HDRG_CHROMA_FIT_SEGMENTED) {
            job.bent = scratch + 5u * sites;

            for (p = 0; p < 3u; p++) {
                job.knot[p] = scratch + (6u + p) * sites;
                job.value[0][p] = scratch + (9u + p) * sites;
                job.value[1][p] = scratch + (12u + p) * sites;
            }
        }

        hdrg_exec_rows(exec, hdrg_yuv_chroma_height(yuv), hdrg_yuv_site_rows, &job);
        hdrg_exec_rows(exec, hdrg_yuv_chroma_height(yuv), hdrg_yuv_slope_rows, &job);
    }

    hdrg_exec_rows(exec, rows, hdrg_yuv_rgb_rows, &job);
}

extern bool hdrg_yuv_valid(const hdrg_yuv_t *yuv)
{
    return yuv->width > 0 && yuv->height > 0 &&
           (yuv->layout == HDRG_YUV_420 || yuv->layout == HDRG_YUV_422 ||
            yuv->layout == HDRG_YUV_444) &&
           (yuv->matrix == HDRG_MATRIX_BT709 || yuv->matrix == HDRG_MATRIX_BT601 ||
            yuv->matrix == HDRG_MATRIX_BT2020) &&
           yuv->bits >= 8 && yuv->bits <= 16 && yuv->siting <= HDRG_CHROMA_BOTTOM &&
           yuv->fit <= HDRG_CHROMA_FIT_SEGMENTED && (!yuv->jfif || yuv->full_range) &&
           (!yuv->msb || yuv->bits > 8);
}

extern bool hdrg_yuv_fits(const hdrg_yuv_t *yuv)
{
    const uint32_t cw = hdrg_yuv_chroma_width(yuv);
    const size_t bytes = (yuv->bits > 8u) ? 2u : 1u;

    if (yuv->interleaved) {
        return yuv->planes[0] != NULL && yuv->planes[1] != NULL &&
               yuv->strides[0] >= (size_t)yuv->width * bytes &&
               yuv->strides[1] >= 2u * (size_t)cw * bytes;
    }

    return yuv->planes[0] != NULL && yuv->planes[1] != NULL && yuv->planes[2] != NULL &&
           yuv->strides[0] >= (size_t)yuv->width * bytes && yuv->strides[1] >= (size_t)cw * bytes &&
           yuv->strides[2] >= (size_t)cw * bytes;
}

extern size_t hdrg_yuv_scratch(const hdrg_yuv_t *yuv)
{
    hdrg_yuv_t band;

    if (yuv->layout == HDRG_YUV_444) {
        return 0;
    }

    /* a band's chroma sites (a picture taller than one: converted band by band) */
    band = *yuv;
    band.height = HDRG_MIN(yuv->height, HDRG_YUV_BAND + 2u * HDRG_YUV_MARGIN);

    return (size_t)hdrg_yuv_chroma_width(&band) * hdrg_yuv_chroma_height(&band) *
           ((yuv->fit == HDRG_CHROMA_FIT_SEGMENTED) ? 15u : 5u);
}

extern void hdrg_yuv_to_rgb16(const hdrg_exec_t *exec, const hdrg_yuv_t *yuv, float *scratch,
                              uint16_t *rgb, size_t stride)
{
    const uint32_t vertical = (yuv->layout == HDRG_YUV_420) ? 2u : 1u;
    hdrg_yuv_t band;
    uint32_t y0, y1, s0, s1, cs;
    uint8_t p;

    /*
     * subsampled chroma through its sites, band by band: each band's rows with the context of
     * HDRG_YUV_MARGIN rows on each side, so every row is the whole picture's (even starts)
     */
    if (yuv->layout == HDRG_YUV_444 || scratch == NULL ||
        yuv->height <= HDRG_YUV_BAND + 2u * HDRG_YUV_MARGIN) {
        hdrg_yuv_band_to_rgb16(exec, yuv, scratch, rgb, stride, 0, yuv->height);

        return;
    }

    for (y0 = 0; y0 < yuv->height; y0 += HDRG_YUV_BAND) {
        y1 = HDRG_MIN(y0 + HDRG_YUV_BAND, yuv->height);
        s0 = (y0 > HDRG_YUV_MARGIN) ? y0 - HDRG_YUV_MARGIN : 0u;
        s1 = HDRG_MIN(y1 + HDRG_YUV_MARGIN, yuv->height);
        cs = s0 / vertical;
        band = *yuv;
        band.height = s1 - s0;
        band.planes[0] = (const uint8_t *)yuv->planes[0] + (size_t)s0 * yuv->strides[0];

        for (p = 1; p < 3u; p++) {
            band.planes[p] = (yuv->planes[p] != NULL)
                                 ? (const uint8_t *)yuv->planes[p] + (size_t)cs * yuv->strides[p]
                                 : NULL;
        }

        hdrg_yuv_band_to_rgb16(exec, &band, scratch,
                               (uint16_t *)(void *)((uint8_t *)rgb + (size_t)s0 * stride), stride,
                               y0 - s0, y1 - y0);
    }
}

extern void hdrg_rgb16_to_yuv420(const hdrg_exec_t *exec, const uint16_t *rgb, uint32_t y0,
                                 uint32_t rows, const hdrg_yuv_t *yuv)
{
    hdrg_yuv_job_t job;

    memset(&job, 0, sizeof(job));
    job.yuv = yuv;
    job.src = rgb;
    job.y0 = y0;
    job.rows = rows;
    hdrg_yuv_matrix(yuv->matrix, &job);

    hdrg_exec_rows(exec, (rows + 1u) / 2u, hdrg_yuv_420_rows, &job);
}

extern void hdrg_rgb16_to_yuv444(const hdrg_exec_t *exec, const uint16_t *rgb, uint32_t y0,
                                 uint32_t rows, const hdrg_yuv_t *yuv)
{
    hdrg_yuv_job_t job;

    memset(&job, 0, sizeof(job));
    job.yuv = yuv;
    job.src = rgb;
    job.y0 = y0;
    job.rows = rows;
    hdrg_yuv_matrix(yuv->matrix, &job);

    hdrg_exec_rows(exec, rows, hdrg_yuv_444_rows, &job);
}

extern void hdrg_yuv_output_coeffs(const hdrg_yuv_t *yuv, float out[10])
{
    hdrg_yuv_range_t range;
    hdrg_yuv_job_t job;

    memset(&job, 0, sizeof(job));
    hdrg_yuv_matrix(yuv->matrix, &job);
    hdrg_yuv_range(yuv, false, &range);

    out[0] = job.kr;
    out[1] = job.kg;
    out[2] = job.kb;
    out[3] = job.inv_cb_b;
    out[4] = job.inv_cr_r;
    out[5] = range.y_off;
    out[6] = range.y_range;
    out[7] = range.c_off;
    out[8] = range.c_range;
    out[9] = range.top;
}

extern void hdrg_yuv_input_coeffs(const hdrg_yuv_t *yuv, float out[8], uint32_t offsets[2])
{
    hdrg_yuv_range_t range;
    hdrg_yuv_job_t job;

    memset(&job, 0, sizeof(job));
    hdrg_yuv_matrix(yuv->matrix, &job);
    hdrg_yuv_range(yuv, true, &range);
    hdrg_yuv_offsets(yuv, &offsets[0], &offsets[1]);

    out[0] = range.y_off;
    out[1] = range.inv_y;
    out[2] = range.c_off;
    out[3] = range.inv_c;
    out[4] = job.cr_r;
    out[5] = job.cb_b;
    out[6] = job.cb_g;
    out[7] = job.cr_g;
}
