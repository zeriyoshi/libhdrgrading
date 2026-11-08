/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "color.h"

#include <math.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"

#define HDRG_CUSP_HUES      256u
#define HDRG_BOUNDARY_STEPS 18u  /* bisection steps of the chroma boundary */
#define HDRG_CUSP_STEPS     28u  /* golden-section steps of the cusp lightness */
#define HDRG_CHROMA_MAX     0.5  /* above any RGB gamut in Oklab */
#define HDRG_GRAY_CHROMA    1e-7 /* below: neutral */
#define HDRG_GAMUT_EPSILON  1e-6 /* RGB tolerance of the boundary test */
#define HDRG_COMPRESS_FROM  0.9  /* compression starts at 90% of the target boundary */
#define HDRG_COMPRESS_POWER 1.2f /* ACES reference gamut compression curve */
#define HDRG_WITHIN_EPSILON 1e-3
#define HDRG_SNAP_PRIMARIES 2e-3 /* XYZ of a primary within this of a standard set */
#define HDRG_SNAP_CURVE     5e-4 /* linear light within this of the sRGB curve */
#define HDRG_SNAP_SAMPLES   64u
#define HDRG_WHITE_XY_LO    0.2 /* chromaticity range accepted for the white of colorants */
#define HDRG_WHITE_XY_HI    0.45
#define HDRG_SINGULAR       1e-6

/* hue windows of the protected colors, as pseudo-angles (see hdrg_pseudo_angle) */
#define HDRG_SKIN_IN_LO 0.2877626 /* 22 deg */
#define HDRG_SKIN_LO    0.3660254 /* 30 deg */
#define HDRG_SKIN_HI    0.9195498 /* 85 deg */
#define HDRG_SKIN_IN_HI 1.0497980 /* 93 deg */
#define HDRG_LEAF_IN_LO 1.2341453 /* 107 deg */
#define HDRG_LEAF_LO    1.3180149 /* 115 deg */
#define HDRG_LEAF_HI    1.6339746 /* 150 deg */
#define HDRG_LEAF_IN_HI 1.7122374 /* 158 deg */
#define HDRG_SKY_IN_LO  2.3375402 /* 207 deg */
#define HDRG_SKY_LO     2.4118365 /* 215 deg */
#define HDRG_SKY_HI     2.9195498 /* 265 deg */
#define HDRG_SKY_IN_HI  3.0497980 /* 273 deg */
#define HDRG_SKIN_RC_LO 0.55      /* skin: cusp-relative chroma below 0.7 */
#define HDRG_SKIN_RC_HI 0.75

typedef struct {
    double rgb_from_lms[9]; /* gamut RGB from cubed LMS */
} hdrg_gamut_t;

typedef struct {
    const hdrg_color_t *src;
    const hdrg_gamut_map_t *map;
    const hdrg_gamut_t *src_gamut, *tgt_gamut;
    const double *cusp; /* source cusp chroma per pseudo-angle */
    const double *tgt_from_xyz;
    const double *out_from_xyz; /* the container's (the target's unless BT.2020) */
    double max_code;
    float *nodes;
} hdrg_lut_job_t;

/* D65 white, XYZ */
static const double hdrg_d65[3] = {0.9504559270516716, 1.0, 1.0890577507598784};

static const double hdrg_bradford[9] = {
    0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296,
};

/* Oklab (Ottosson 2020): XYZ -> LMS -> cube root -> Lab, and the inverses */
static const double hdrg_oklab_m1[9] = {
    0.8189330101, 0.3618667424, -0.1288597137, 0.0329845436, 0.9293118715,
    0.0361456387, 0.0482003018, 0.2643662691,  0.6338517070,
};

static const double hdrg_oklab_m2[9] = {
    0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951,  -2.4285922050,
    0.4505937099, 0.0259040371, 0.7827717662,  -0.8086757660,
};

static const double hdrg_oklab_m1_inv[9] = {
    1.2270138511,  -0.5577999807, 0.2812561490,  -0.0405801784, 1.1122568696,
    -0.0716766787, -0.0763812845, -0.4214819784, 1.5861632204,
};

static const double hdrg_oklab_m2_inv[9] = {
    1.0,           0.3963377774, 0.2158037573,  1.0,           -0.1055613458,
    -0.0638541728, 1.0,          -0.0894841775, -1.2914855480,
};

/* xy chromaticities {rx, ry, gx, gy, bx, by, wx, wy} of the known primaries */
static const double hdrg_xy_bt709[8] = {0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290};
static const double hdrg_xy_bt601_625[8] = {0.64, 0.33, 0.29, 0.60, 0.15, 0.06, 0.3127, 0.3290};
static const double hdrg_xy_bt601_525[8] = {0.630, 0.340, 0.310,  0.595,
                                            0.155, 0.070, 0.3127, 0.3290};
static const double hdrg_xy_bt2020[8] = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};
static const double hdrg_xy_dci_p3[8] = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.314, 0.351};
static const double hdrg_xy_p3_d65[8] = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290};
static const double hdrg_xy_adobe[8] = {0.64, 0.33, 0.21, 0.71, 0.15, 0.06, 0.3127, 0.3290};
static const double hdrg_xy_prophoto[8] = {0.7347, 0.2653, 0.1596, 0.8404,
                                           0.0366, 0.0001, 0.3457, 0.3585};

/* the xy primaries and white of HDRG_GAMUT_* */
static inline const double *hdrg_gamut_xy(uint8_t gamut)
{
    if (gamut == HDRG_GAMUT_BT2020) {
        return hdrg_xy_bt2020;
    }

    return (gamut == HDRG_GAMUT_P3) ? hdrg_xy_p3_d65 : hdrg_xy_bt709;
}

/* per preset (PHOTO, ANIME, GAME): r_L knee, r_c ramp, D(L) ramp */
static const float hdrg_map_shapes[3][5] = {
    {0.75f, 0.55f, 0.9f, 0.15f, 0.35f},
    {0.6f, 0.6f, 0.95f, 0.25f, 0.45f},
    {0.65f, 0.5f, 0.9f, 0.05f, 0.2f},
};

/* clamp to [0, 1]; NaN -> 0 */
static inline double hdrg_unit(double v)
{
    return (v >= 0.0) ? ((v <= 1.0) ? v : 1.0) : 0.0;
}

static inline void hdrg_mat3_mul(const double a[9], const double b[9], double out[9])
{
    uint8_t i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        }
    }
}

static inline void hdrg_mat3_vec(const double m[9], const double v[3], double out[3])
{
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    out[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    out[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

static inline double hdrg_smooth(double lo, double hi, double v)
{
    double t;

    t = (v - lo) / (hi - lo);
    t = (t < 0.0) ? 0.0 : ((t > 1.0) ? 1.0 : t);

    return t * t * (3.0 - 2.0 * t);
}

/* rises over [in_lo, lo], falls over [hi, in_hi] */
static inline double hdrg_window(double v, double in_lo, double lo, double hi, double in_hi)
{
    return hdrg_smooth(in_lo, lo, v) * (1.0 - hdrg_smooth(hi, in_hi, v));
}

/* cube root in double: the float root refined by one Newton step */
static inline double hdrg_cbrt(double v)
{
    double x;

    x = (double)hdrg_math_cbrtf((float)v);
    if (x == 0.0) {
        return 0.0;
    }

    return x - (x * x * x - v) / (3.0 * x * x);
}

/*
 * Pseudo-angle of the direction (x, y) in [0, 4), monotonic in the angle:
 * hue windows and the cusp table use it, so no trigonometry is needed
 */
static inline double hdrg_pseudo_angle(double x, double y)
{
    if (y >= 0.0) {
        return (x >= 0.0) ? y / (x + y) : 1.0 - x / (y - x);
    }

    return (x < 0.0) ? 2.0 - y / (-x - y) : 3.0 + x / (x - y);
}

/* unit direction of a pseudo-angle */
static inline void hdrg_pseudo_direction(double p, double dir[2])
{
    double x, y, len;

    if (p < 1.0) {
        x = 1.0 - p;
        y = p;
    } else if (p < 2.0) {
        x = 1.0 - p;
        y = 2.0 - p;
    } else if (p < 3.0) {
        x = p - 3.0;
        y = 2.0 - p;
    } else {
        x = p - 3.0;
        y = p - 4.0;
    }

    len = sqrt(x * x + y * y);
    dir[0] = x / len;
    dir[1] = y / len;
}

static bool hdrg_mat3_inv(const double m[9], double out[9])
{
    double det;

    det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
          m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (det == 0.0) {
        return false;
    }

    out[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    out[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    out[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    out[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    out[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    out[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    out[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    out[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    out[8] = (m[0] * m[4] - m[1] * m[3]) / det;

    return true;
}

/* Bradford adaptation matrix from white src to white dst (XYZ) */
static void hdrg_adapt(const double src[3], const double dst[3], double out[9])
{
    double inv[9], scale[9], tmp[9], s[3], d[3];

    hdrg_mat3_vec(hdrg_bradford, src, s);
    hdrg_mat3_vec(hdrg_bradford, dst, d);
    memset(scale, 0, sizeof(scale));
    scale[0] = d[0] / s[0];
    scale[4] = d[1] / s[1];
    scale[8] = d[2] / s[2];

    (void)hdrg_mat3_inv(hdrg_bradford, inv);
    hdrg_mat3_mul(scale, hdrg_bradford, tmp);
    hdrg_mat3_mul(inv, tmp, out);
}

/* the RGB cube of a gamut (linear RGB -> XYZ matrix) seen from Oklab */
static void hdrg_gamut_init(const double to_xyz[9], hdrg_gamut_t *out)
{
    double from_xyz[9];

    (void)hdrg_mat3_inv(to_xyz, from_xyz);
    hdrg_mat3_mul(from_xyz, hdrg_oklab_m1_inv, out->rgb_from_lms);
}

static bool hdrg_gamut_contains(const hdrg_gamut_t *gamut, double l, double a, double b)
{
    uint8_t c;
    double lab[3], lms[3], rgb[3];

    lab[0] = l;
    lab[1] = a;
    lab[2] = b;
    hdrg_mat3_vec(hdrg_oklab_m2_inv, lab, lms);

    for (c = 0; c < 3; c++) {
        lms[c] = lms[c] * lms[c] * lms[c];
    }

    hdrg_mat3_vec(gamut->rgb_from_lms, lms, rgb);

    return rgb[0] >= -HDRG_GAMUT_EPSILON && rgb[0] <= 1.0 + HDRG_GAMUT_EPSILON &&
           rgb[1] >= -HDRG_GAMUT_EPSILON && rgb[1] <= 1.0 + HDRG_GAMUT_EPSILON &&
           rgb[2] >= -HDRG_GAMUT_EPSILON && rgb[2] <= 1.0 + HDRG_GAMUT_EPSILON;
}

/* largest chroma of the gamut at lightness l in direction dir (bisection) */
static double hdrg_gamut_chroma(const hdrg_gamut_t *gamut, double l, const double dir[2])
{
    uint8_t i;
    double lo, hi, mid;

    if (l <= 0.0 || l >= 1.0) {
        return 0.0;
    }

    lo = 0.0;
    hi = HDRG_CHROMA_MAX;
    for (i = 0; i < HDRG_BOUNDARY_STEPS; i++) {
        mid = 0.5 * (lo + hi);
        if (hdrg_gamut_contains(gamut, l, mid * dir[0], mid * dir[1])) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    return lo;
}

/* cusp (largest chroma over all lightness) per pseudo-angle, golden-section search */
static void hdrg_gamut_cusps(const hdrg_gamut_t *gamut, double *cusp)
{
    const double ratio = 0.6180339887498949;
    uint32_t h;
    uint8_t i;
    double dir[2], lo, hi, x1, x2, c1, c2;

    for (h = 0; h < HDRG_CUSP_HUES; h++) {
        hdrg_pseudo_direction(4.0 * (double)h / (double)HDRG_CUSP_HUES, dir);

        lo = 0.0;
        hi = 1.0;
        x1 = hi - ratio * (hi - lo);
        x2 = lo + ratio * (hi - lo);
        c1 = hdrg_gamut_chroma(gamut, x1, dir);
        c2 = hdrg_gamut_chroma(gamut, x2, dir);
        for (i = 0; i < HDRG_CUSP_STEPS; i++) {
            if (c1 < c2) {
                lo = x1;
                x1 = x2;
                c1 = c2;
                x2 = lo + ratio * (hi - lo);
                c2 = hdrg_gamut_chroma(gamut, x2, dir);
            } else {
                hi = x2;
                x2 = x1;
                c2 = c1;
                x1 = hi - ratio * (hi - lo);
                c1 = hdrg_gamut_chroma(gamut, x1, dir);
            }
        }

        cusp[h] = HDRG_MAX(c1, c2);
    }
}

/* cusp chroma at a pseudo-angle, linear between the table's hues */
static double hdrg_cusp_at(const double *cusp, double p)
{
    uint32_t i0, i1;
    double x, t;

    x = p * (double)HDRG_CUSP_HUES / 4.0;
    i0 = (uint32_t)x % HDRG_CUSP_HUES;
    i1 = (i0 + 1u) % HDRG_CUSP_HUES;
    t = x - (double)(uint32_t)x;

    return cusp[i0] + t * (cusp[i1] - cusp[i0]);
}

/* weight w of the expansion rule at chroma c (see hdrg_gamut_map_t) */
static double hdrg_expand_weight(const hdrg_gamut_map_t *map, double l, double p, double r_l,
                                 double r_c)
{
    double w, skin, memory;

    skin = hdrg_window(p, HDRG_SKIN_IN_LO, HDRG_SKIN_LO, HDRG_SKIN_HI, HDRG_SKIN_IN_HI) *
           (1.0 - hdrg_smooth(HDRG_SKIN_RC_LO, HDRG_SKIN_RC_HI, r_c));
    memory = HDRG_MAX(hdrg_window(p, HDRG_LEAF_IN_LO, HDRG_LEAF_LO, HDRG_LEAF_HI, HDRG_LEAF_IN_HI),
                      hdrg_window(p, HDRG_SKY_IN_LO, HDRG_SKY_LO, HDRG_SKY_HI, HDRG_SKY_IN_HI));

    w = (double)map->strength * hdrg_smooth((double)map->knee, 1.0, r_l) *
        hdrg_smooth((double)map->cusp_lo, (double)map->cusp_hi, r_c);
    w *= 1.0 - (double)map->skin * skin;
    w *= 1.0 - (double)map->memory * memory;
    w *= hdrg_smooth((double)map->dark_lo, (double)map->dark_hi, l);

    return w;
}

/*
 * ACES reference gamut compression of x = chroma / target boundary: the
 * identity below HDRG_COMPRESS_FROM, [from, limit] onto [from, 1] above it
 */
static double hdrg_compress(double x, double limit)
{
    const double from = HDRG_COMPRESS_FROM;
    double r, y, s, u;

    if (x <= from || limit <= 1.0) {
        return HDRG_MIN(x, 1.0);
    }

    r = (limit - from) / (1.0 - from);
    y = (double)hdrg_math_powf(1.0f - hdrg_math_powf((float)r, -HDRG_COMPRESS_POWER),
                               1.0f / HDRG_COMPRESS_POWER);
    s = (1.0 - from) / y;
    u = (x - from) / s;

    return from + s * u /
                      (double)hdrg_math_powf(1.0f + hdrg_math_powf((float)u, HDRG_COMPRESS_POWER),
                                             1.0f / HDRG_COMPRESS_POWER);
}

/* one LUT node: source linear rgb -> target linear rgb */
static void hdrg_map_node(const hdrg_lut_job_t *job, const double rgb[3], double out[3])
{
    const hdrg_gamut_map_t *map = job->map;
    uint8_t i;
    bool inside;
    double xyz[3], lab[3], dir[2], c, p, cmax_src, cmax_tgt, cusp, k, w, c1, c1_max, x;

    hdrg_mat3_vec(job->src->to_xyz, rgb, xyz);
    hdrg_mat3_vec(job->tgt_from_xyz, xyz, out);

    inside = true;
    for (i = 0; i < 3; i++) {
        inside = inside && out[i] >= -HDRG_GAMUT_EPSILON && out[i] <= 1.0 + HDRG_GAMUT_EPSILON;
    }

    /* neutrals keep their value: the whites coincide */
    if (rgb[0] == rgb[1] && rgb[1] == rgb[2]) {
        out[0] = rgb[0];
        out[1] = rgb[0];
        out[2] = rgb[0];

        return;
    }

    hdrg_oklab_from_xyz(xyz, lab);
    c = sqrt(lab[1] * lab[1] + lab[2] * lab[2]);

    if (c > HDRG_GRAY_CHROMA) {
        dir[0] = lab[1] / c;
        dir[1] = lab[2] / c;
        p = hdrg_pseudo_angle(dir[0], dir[1]);
        cmax_src = HDRG_MAX(hdrg_gamut_chroma(job->src_gamut, lab[0], dir), c);
        cmax_tgt = hdrg_gamut_chroma(job->tgt_gamut, lab[0], dir);
        cusp = HDRG_MAX(hdrg_cusp_at(job->cusp, p), cmax_src);

        /*
         * A ray at fixed L and hue can leave the RGB cube and come back at an
         * edge (the blue edges: out from 85% of the chroma): the bisection
         * finds the first exit, so a color the target holds bounds it itself
         */
        if (inside) {
            cmax_tgt = HDRG_MAX(cmax_tgt, c);
        }

        k = (cmax_src > 0.0) ? cmax_tgt / cmax_src : 1.0;

        w = hdrg_expand_weight(map, lab[0], p, c / cmax_src, c / cusp);
        c1 = c * (1.0 + w * (double)map->reach * HDRG_MAX(k - 1.0, 0.0)) * (double)map->saturation;

        /* the farthest this (L, h) reaches: the source boundary, mapped */
        w = hdrg_expand_weight(map, lab[0], p, 1.0, cmax_src / cusp);
        c1_max = cmax_src * (1.0 + w * (double)map->reach * HDRG_MAX(k - 1.0, 0.0)) *
                 (double)map->saturation;

        x = (cmax_tgt > 0.0) ? hdrg_compress(c1 / cmax_tgt, c1_max / cmax_tgt) * cmax_tgt : 0.0;
        lab[1] = dir[0] * x;
        lab[2] = dir[1] * x;
        hdrg_oklab_to_xyz(lab, xyz);
    }

    hdrg_mat3_vec(job->out_from_xyz, xyz, out);

    /* Display P3's red lies a hair outside BT.2020's red-green edge */
    for (i = 0; map->container != map->target && i < 3; i++) {
        out[i] = HDRG_MAX(out[i], 0.0);
    }
}

static void hdrg_lut_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_lut_job_t *job = (const hdrg_lut_job_t *)arg;
    const uint32_t n = HDRG_LUT_SIZE;
    uint32_t row, b;
    uint8_t c;
    double rgb[3], out[3];
    float *node;

    for (row = row_begin; row < row_end; row++) {
        rgb[0] = hdrg_curve_eval(&job->src->curve[0], (double)(row / n) / (double)(n - 1u));
        rgb[1] = hdrg_curve_eval(&job->src->curve[1], (double)(row % n) / (double)(n - 1u));
        for (b = 0; b < n; b++) {
            rgb[2] = hdrg_curve_eval(&job->src->curve[2], (double)b / (double)(n - 1u));

            hdrg_map_node(job, rgb, out);

            node = job->nodes + ((size_t)row * n + b) * 3u;
            for (c = 0; c < 3; c++) {
                node[c] = (float)(hdrg_curve_inverse(&job->src->curve[c], out[c]) * job->max_code);
            }
        }
    }
}

extern void hdrg_oklab_from_xyz(const double xyz[3], double lab[3])
{
    uint8_t c;
    double lms[3];

    hdrg_mat3_vec(hdrg_oklab_m1, xyz, lms);

    for (c = 0; c < 3; c++) {
        lms[c] = hdrg_cbrt(lms[c]);
    }

    hdrg_mat3_vec(hdrg_oklab_m2, lms, lab);
}

extern void hdrg_oklab_to_xyz(const double lab[3], double xyz[3])
{
    uint8_t c;
    double lms[3];

    hdrg_mat3_vec(hdrg_oklab_m2_inv, lab, lms);

    for (c = 0; c < 3; c++) {
        lms[c] = lms[c] * lms[c] * lms[c];
    }

    hdrg_mat3_vec(hdrg_oklab_m1_inv, lms, xyz);
}

extern void hdrg_curve_set_gamma(double g, hdrg_curve_t *out)
{
    memset(out, 0, sizeof(*out));

    out->type = HDRG_CURVE_GAMMA;
    out->g = g;
}

extern void hdrg_curve_set_srgb(hdrg_curve_t *out)
{
    memset(out, 0, sizeof(*out));

    out->type = HDRG_CURVE_SRGB;
}

extern double hdrg_curve_eval(const hdrg_curve_t *curve, double x)
{
    uint32_t i;
    double pos, t;

    x = hdrg_unit(x);

    switch (curve->type) {
    case HDRG_CURVE_SRGB:
        return (double)hdrg_math_srgb_eotf((float)x);
    case HDRG_CURVE_PARA:
        if (x < curve->d) {
            return hdrg_unit(curve->c * x + curve->f);
        }

        t = curve->a * x + curve->b;

        return hdrg_unit(((t > 0.0) ? (double)hdrg_math_powf((float)t, (float)curve->g) : 0.0) +
                         curve->e);
    case HDRG_CURVE_TABLE:
        if (curve->count < 2) {
            return x;
        }

        pos = x * (double)(curve->count - 1u);
        i = HDRG_MIN((uint32_t)pos, curve->count - 2u);
        t = pos - (double)i;

        return (double)curve->table[i] +
               t * ((double)curve->table[i + 1u] - (double)curve->table[i]);
    case HDRG_CURVE_GAMMA:
    default:
        if (curve->g == 1.0 || x == 0.0) {
            return x;
        }

        return hdrg_unit((double)hdrg_math_powf((float)x, (float)curve->g));
    }
}

extern double hdrg_curve_inverse(const hdrg_curve_t *curve, double y)
{
    uint32_t lo, hi, mid;
    double t, x;

    y = hdrg_unit(y);

    switch (curve->type) {
    case HDRG_CURVE_SRGB:
        return (double)hdrg_math_srgb_oetf((float)y);
    case HDRG_CURVE_PARA:
        if (y < curve->c * curve->d + curve->f && curve->c > 0.0) {
            x = (y - curve->f) / curve->c;
        } else {
            t = y - curve->e;
            x = (t > 0.0 && curve->a > 0.0)
                    ? ((double)hdrg_math_powf((float)t, (float)(1.0 / curve->g)) - curve->b) /
                          curve->a
                    : curve->d;
        }

        return hdrg_unit(x);
    case HDRG_CURVE_TABLE:
        if (curve->count < 2) {
            return y;
        }

        if (y <= (double)curve->table[0]) {
            return 0.0;
        }

        if (y >= (double)curve->table[curve->count - 1u]) {
            return 1.0;
        }

        /* the first segment reaching y */
        lo = 0;
        hi = curve->count - 1u;
        while (hi - lo > 1u) {
            mid = lo + (hi - lo) / 2u;
            if ((double)curve->table[mid] < y) {
                lo = mid;
            } else {
                hi = mid;
            }
        }

        t = (double)curve->table[hi] - (double)curve->table[lo];
        x = (double)lo + ((t > 0.0) ? (y - (double)curve->table[lo]) / t : 0.0);

        return x / (double)(curve->count - 1u);
    case HDRG_CURVE_GAMMA:
    default:
        if (curve->g == 1.0 || y == 0.0) {
            return y;
        }

        return hdrg_unit((double)hdrg_math_powf((float)y, (float)(1.0 / curve->g)));
    }
}

extern bool hdrg_color_set_xy(const double xy[8], hdrg_color_t *out)
{
    uint8_t i;
    double m[9], inv[9], w[3], s[3], adapt[9], tmp[9];

    for (i = 0; i < 4; i++) {
        if (!(xy[i * 2] >= 0.0 && xy[i * 2 + 1] > 0.0 && xy[i * 2] + xy[i * 2 + 1] <= 1.0)) {
            return false;
        }
    }

    for (i = 0; i < 3; i++) {
        m[i] = xy[i * 2] / xy[i * 2 + 1];
        m[3 + i] = 1.0;
        m[6 + i] = (1.0 - xy[i * 2] - xy[i * 2 + 1]) / xy[i * 2 + 1];
    }

    w[0] = xy[6] / xy[7];
    w[1] = 1.0;
    w[2] = (1.0 - xy[6] - xy[7]) / xy[7];

    /* primaries on one line or a white outside them: unusable, as for colorants */
    if (!hdrg_mat3_inv(m, inv)) {
        return false;
    }

    for (i = 0; i < 9; i++) {
        if (!(fabs(inv[i]) < 1.0 / HDRG_SINGULAR)) {
            return false;
        }
    }

    hdrg_mat3_vec(inv, w, s);

    for (i = 0; i < 3; i++) {
        if (!(s[i] > 0.0)) {
            return false;
        }
    }

    for (i = 0; i < 9; i++) {
        m[i] *= s[i % 3];
    }

    /* another white (ProPhoto D50, DCI) is adapted to D65 */
    hdrg_adapt(w, hdrg_d65, adapt);
    hdrg_mat3_mul(adapt, m, tmp);
    memcpy(out->to_xyz, tmp, sizeof(tmp));

    return true;
}

extern bool hdrg_color_set_colorants(const double colorants[9], hdrg_color_t *out)
{
    uint8_t i;
    double white[3], sum, scale, adapt[9], inv[9];

    for (i = 0; i < 3; i++) {
        white[i] = colorants[i * 3] + colorants[i * 3 + 1] + colorants[i * 3 + 2];
    }

    sum = white[0] + white[1] + white[2];
    if (!(white[1] > 0.0 && sum > 0.0) || !(white[0] / sum >= HDRG_WHITE_XY_LO) ||
        white[0] / sum > HDRG_WHITE_XY_HI || white[1] / sum < HDRG_WHITE_XY_LO ||
        white[1] / sum > HDRG_WHITE_XY_HI) {
        return false;
    }

    if (!hdrg_mat3_inv(colorants, inv)) {
        return false;
    }

    for (i = 0; i < 9; i++) {
        if (!(fabs(inv[i]) < 1.0 / HDRG_SINGULAR)) {
            return false;
        }
    }

    /* white Y = 1, then the white the colorants sum to (D50 for ICC) -> D65 */
    scale = 1.0 / white[1];
    for (i = 0; i < 3; i++) {
        white[i] *= scale;
    }

    hdrg_adapt(white, hdrg_d65, adapt);
    hdrg_mat3_mul(adapt, colorants, out->to_xyz);

    for (i = 0; i < 9; i++) {
        out->to_xyz[i] *= scale;
    }

    return true;
}

extern bool hdrg_color_from_codes(uint8_t primaries, uint8_t transfer, hdrg_color_t *out)
{
    const double *xy;
    uint8_t c;

    memset(out, 0, sizeof(*out));

    switch (primaries) {
    case HDRG_PRIMARIES_BT709:
        xy = hdrg_xy_bt709;

        break;
    case HDRG_PRIMARIES_BT601_625:
        xy = hdrg_xy_bt601_625;

        break;
    case HDRG_PRIMARIES_BT601_525:
        xy = hdrg_xy_bt601_525;

        break;
    case HDRG_PRIMARIES_BT2020:
        xy = hdrg_xy_bt2020;

        break;
    case HDRG_PRIMARIES_DCI_P3:
        xy = hdrg_xy_dci_p3;

        break;
    case HDRG_PRIMARIES_P3_D65:
        xy = hdrg_xy_p3_d65;

        break;
    case HDRG_PRIMARIES_ADOBE_RGB:
        xy = hdrg_xy_adobe;

        break;
    case HDRG_PRIMARIES_PROPHOTO:
        xy = hdrg_xy_prophoto;

        break;
    default:
        return false;
    }

    (void)hdrg_color_set_xy(xy, out);

    for (c = 0; c < 3; c++) {
        switch (transfer) {
        case HDRG_TRANSFER_SRGB:
            hdrg_curve_set_srgb(&out->curve[c]);
            out->srgb_curve = true;

            break;
        case HDRG_TRANSFER_BT709:
        case HDRG_TRANSFER_BT601:
        case HDRG_TRANSFER_BT2020_10:
        case HDRG_TRANSFER_BT2020_12:
            /* camera OETFs: displayed with the BT.1886 reference EOTF */
            hdrg_curve_set_gamma(2.4, &out->curve[c]);

            break;
        case HDRG_TRANSFER_GAMMA22:
            hdrg_curve_set_gamma(2.2, &out->curve[c]);

            break;
        case HDRG_TRANSFER_GAMMA28:
            hdrg_curve_set_gamma(2.8, &out->curve[c]);

            break;
        case HDRG_TRANSFER_LINEAR:
            hdrg_curve_set_gamma(1.0, &out->curve[c]);

            break;
        case HDRG_TRANSFER_ADOBE_RGB:
            hdrg_curve_set_gamma(563.0 / 256.0, &out->curve[c]);

            break;
        case HDRG_TRANSFER_PROPHOTO:
            memset(&out->curve[c], 0, sizeof(out->curve[c]));
            out->curve[c].type = HDRG_CURVE_PARA;
            out->curve[c].g = 1.8;
            out->curve[c].a = 1.0;
            out->curve[c].c = 1.0 / 16.0;
            out->curve[c].d = 1.0 / 32.0;

            break;
        default:
            return false;
        }
    }

    out->transfer = transfer;

    return true;
}

extern void hdrg_color_describe(const hdrg_color_t *color, double xy[8], uint32_t *transfer)
{
    const double *m = color->to_xyz;
    uint8_t k;
    double sum, white[3];

    white[0] = 0.0;
    white[1] = 0.0;
    white[2] = 0.0;

    for (k = 0; k < 3u; k++) {
        sum = m[k] + m[3u + k] + m[6u + k];
        xy[2u * k] = (sum > 0.0) ? m[k] / sum : 0.0;
        xy[2u * k + 1u] = (sum > 0.0) ? m[3u + k] / sum : 0.0;
        white[0] += m[k];
        white[1] += m[3u + k];
        white[2] += m[6u + k];
    }

    sum = white[0] + white[1] + white[2];
    xy[6] = (sum > 0.0) ? white[0] / sum : 0.0;
    xy[7] = (sum > 0.0) ? white[1] / sum : 0.0;
    *transfer = (color->transfer != 0) ? color->transfer : (color->srgb_curve ? 13u : 2u);
}

extern void hdrg_color_snap(const hdrg_allocator_t *alloc, hdrg_color_t *color)
{
    const double *standards[4] = {hdrg_xy_bt709, hdrg_xy_p3_d65, hdrg_xy_adobe, hdrg_xy_bt2020};
    hdrg_color_t reference;
    uint32_t i;
    uint8_t s, c;
    bool srgb;
    double diff;

    for (s = 0; s < 4; s++) {
        (void)hdrg_color_set_xy(standards[s], &reference);

        diff = 0.0;
        for (i = 0; i < 9; i++) {
            diff = HDRG_MAX(diff, fabs(reference.to_xyz[i] - color->to_xyz[i]));
        }

        if (diff <= HDRG_SNAP_PRIMARIES) {
            memcpy(color->to_xyz, reference.to_xyz, sizeof(color->to_xyz));

            break;
        }
    }

    color->srgb_curve = true;

    for (c = 0; c < 3; c++) {
        srgb = color->curve[c].type == HDRG_CURVE_SRGB;
        for (i = 0; i <= HDRG_SNAP_SAMPLES && !srgb; i++) {
            diff = fabs(hdrg_curve_eval(&color->curve[c], (double)i / HDRG_SNAP_SAMPLES) -
                        (double)hdrg_math_srgb_eotf((float)i / (float)HDRG_SNAP_SAMPLES));
            if (diff > HDRG_SNAP_CURVE) {
                break;
            }
        }

        if (srgb || i > HDRG_SNAP_SAMPLES) {
            hdrg_mem_free(alloc, color->curve[c].table);
            hdrg_curve_set_srgb(&color->curve[c]);
        } else {
            color->srgb_curve = false;
        }
    }
}

extern void hdrg_color_set_sdr_eotf(hdrg_color_t *color, uint8_t sdr_eotf)
{
    uint8_t c;

    if (!color->srgb_curve ||
        (sdr_eotf != HDRG_SDR_EOTF_GAMMA22 && sdr_eotf != HDRG_SDR_EOTF_BT1886)) {
        return;
    }

    for (c = 0; c < 3; c++) {
        hdrg_curve_set_gamma((sdr_eotf == HDRG_SDR_EOTF_GAMMA22) ? 2.2 : 2.4, &color->curve[c]);
    }

    color->srgb_curve = false;
}

extern hdrg_status_t hdrg_color_copy(const hdrg_allocator_t *alloc, const hdrg_color_t *src,
                                     hdrg_color_t *dst)
{
    uint8_t c;

    memcpy(dst, src, sizeof(*dst));

    for (c = 0; c < 3; c++) {
        dst->curve[c].table = NULL;
    }

    for (c = 0; c < 3; c++) {
        if (src->curve[c].type != HDRG_CURVE_TABLE || src->curve[c].table == NULL) {
            continue;
        }

        dst->curve[c].table =
            (float *)hdrg_mem_alloc(alloc, (size_t)src->curve[c].count * sizeof(float));
        if (dst->curve[c].table == NULL) {
            hdrg_color_free(alloc, dst);

            return HDRG_ERR_NOMEM;
        }

        memcpy(dst->curve[c].table, src->curve[c].table,
               (size_t)src->curve[c].count * sizeof(float));
    }

    return HDRG_OK;
}

extern void hdrg_color_free(const hdrg_allocator_t *alloc, hdrg_color_t *color)
{
    uint8_t c;

    if (color == NULL) {
        return;
    }

    for (c = 0; c < 3; c++) {
        hdrg_mem_free(alloc, color->curve[c].table);
        color->curve[c].table = NULL;
    }
}

extern void hdrg_color_luma(const hdrg_color_t *color, float coeffs[3])
{
    coeffs[0] = (float)color->to_xyz[3];
    coeffs[1] = (float)color->to_xyz[4];
    coeffs[2] = (float)color->to_xyz[5];
}

extern bool hdrg_color_within(const hdrg_color_t *color, uint8_t gamut)
{
    hdrg_color_t target;
    uint8_t i;
    double from_xyz[9], m[9];

    (void)hdrg_color_set_xy(hdrg_gamut_xy(gamut), &target);
    (void)hdrg_mat3_inv(target.to_xyz, from_xyz);
    hdrg_mat3_mul(from_xyz, color->to_xyz, m);

    /* every source primary has non-negative target components */
    for (i = 0; i < 9; i++) {
        if (m[i] < -HDRG_WITHIN_EPSILON) {
            return false;
        }
    }

    return true;
}

extern void hdrg_color_to_gamut(const hdrg_color_t *color, uint8_t gamut, float out[9])
{
    hdrg_color_t target;
    uint8_t i;
    double from_xyz[9], m[9];

    (void)hdrg_color_set_xy(hdrg_gamut_xy(gamut), &target);
    (void)hdrg_mat3_inv(target.to_xyz, from_xyz);
    hdrg_mat3_mul(from_xyz, color->to_xyz, m);

    for (i = 0; i < 9u; i++) {
        out[i] = (float)m[i];
    }
}

extern void hdrg_gamut_luma(uint8_t gamut, float coeffs[3])
{
    hdrg_color_t target;

    (void)hdrg_color_set_xy(hdrg_gamut_xy(gamut), &target);
    hdrg_color_luma(&target, coeffs);
}

extern void hdrg_gamut_map_init(const hdrg_params_t *params, uint8_t target, hdrg_gamut_map_t *out)
{
    const float *shape = hdrg_map_shapes[HDRG_MIN(params->preset, HDRG_PRESET_GAME)];

    out->strength = params->expand_strength;
    out->reach = params->expand_reach;
    out->skin = params->expand_skin;
    out->memory = params->expand_memory;
    out->saturation = params->saturation;
    out->knee = shape[0];
    out->cusp_lo = shape[1];
    out->cusp_hi = shape[2];
    out->dark_lo = shape[3];
    out->dark_hi = shape[4];
    out->target = (target == HDRG_GAMUT_BT2020) ? HDRG_GAMUT_P3 : target;
    out->container = target;
}

extern hdrg_status_t hdrg_color_lut_build(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                          const hdrg_color_t *src, const float *eotf,
                                          uint32_t max_code, const hdrg_gamut_map_t *map,
                                          hdrg_lut_t *out)
{
    const uint32_t n = HDRG_LUT_SIZE;
    hdrg_color_t target, container;
    hdrg_gamut_t src_gamut, tgt_gamut;
    hdrg_lut_job_t job;
    double cusp[HDRG_CUSP_HUES], tgt_from_xyz[9], out_from_xyz[9];

    memset(out, 0, sizeof(*out));

    out->nodes = (float *)hdrg_mem_alloc(alloc, (size_t)n * n * n * 3u * sizeof(float));
    if (out->nodes == NULL) {
        return HDRG_ERR_NOMEM;
    }

    out->eotf = eotf;
    out->entries = max_code + 1u;
    out->max_code = (float)max_code;

    (void)hdrg_color_set_xy(hdrg_gamut_xy(map->target), &target);
    (void)hdrg_color_set_xy(hdrg_gamut_xy(map->container), &container);
    (void)hdrg_mat3_inv(target.to_xyz, tgt_from_xyz);
    (void)hdrg_mat3_inv(container.to_xyz, out_from_xyz);
    hdrg_gamut_init(src->to_xyz, &src_gamut);
    hdrg_gamut_init(target.to_xyz, &tgt_gamut);
    hdrg_gamut_cusps(&src_gamut, cusp);

    job.src = src;
    job.map = map;
    job.src_gamut = &src_gamut;
    job.tgt_gamut = &tgt_gamut;
    job.cusp = cusp;
    job.tgt_from_xyz = tgt_from_xyz;
    job.out_from_xyz = out_from_xyz;
    job.max_code = (double)max_code;
    job.nodes = out->nodes;

    hdrg_exec_rows(exec, n * n, hdrg_lut_rows, &job);

    return HDRG_OK;
}

extern void hdrg_color_lut_free(const hdrg_allocator_t *alloc, hdrg_lut_t *lut)
{
    hdrg_mem_free(alloc, lut->nodes);

    lut->nodes = NULL;
}
