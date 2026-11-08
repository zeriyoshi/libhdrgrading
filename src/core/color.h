/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_COLOR_H
#define HDRG_CORE_COLOR_H

#include <stdbool.h>
#include <stdint.h>

#include "../priv.h"
#include "exec.h"

/* transfer curve kinds: code value in [0, 1] -> linear light */
#define HDRG_CURVE_GAMMA 1u /* x^g (g = 1: linear) */
#define HDRG_CURVE_SRGB  2u /* IEC 61966-2-1 piecewise */
#define HDRG_CURVE_PARA  3u /* ICC parametricCurveType function 4 (covers 0-3) */
#define HDRG_CURVE_TABLE 4u /* ICC curveType table, linear interpolation */

typedef struct {
    float *table; /* TABLE: count samples over [0, 1], owned */
    uint32_t count;
    double g, a, b, c, d, e, f; /* GAMMA: g; PARA: y = (a x + b)^g + e for x >= d, else c x + f */
    uint8_t type;
} hdrg_curve_t;

/*
 * Color of source code values: one transfer curve per channel and the
 * matrix from linear RGB to D65-relative CIE XYZ (white maps to D65 with
 * Y = 1; a source with another white is chromatically adapted to it).
 */
typedef struct {
    hdrg_curve_t curve[3];
    double to_xyz[9];
    uint8_t transfer; /* the HDRG_TRANSFER_* it was made from; 0 = a curve of its own */
    bool srgb_curve;  /* the IEC 61966-2-1 curve on all channels: sdr_eotf may replace it */
} hdrg_color_t;

/*
 * Gamut mapping in Oklab LCh, L and hue fixed:
 *   r_L = C / Cmax_src(L,h), r_c = C / Ccusp_src(h), k = Cmax_tgt(L,h) / Cmax_src(L,h)
 *   w   = strength * S(r_L; knee, 1) * S(r_c; cusp_lo, cusp_hi) * (1 - skin * skin(h, r_c))
 *         * (1 - memory * mem(h)) * D(L)
 *   C'  = C * (1 + w * reach * max(k - 1, 0)) * saturation
 * then a soft compression toward the target boundary (from 90% of it), so
 * sources wider than the target and saturation above 1 stay in gamut.
 */
typedef struct {
    float strength, reach, skin, memory, saturation;
    float knee;             /* r_L onset */
    float cusp_lo, cusp_hi; /* r_c ramp */
    float dark_lo, dark_hi; /* D(L) ramp on Oklab L */
    uint8_t target;         /* HDRG_GAMUT_SRGB / P3: the boundary the mapping works toward */
    uint8_t container;      /* HDRG_GAMUT_*: the primaries the mapped colors are written in */
} hdrg_gamut_map_t;

/* H.273-style codes (HDRG_PRIMARIES_* / HDRG_TRANSFER_*); false = unknown */
extern bool hdrg_color_from_codes(uint8_t primaries, uint8_t transfer, hdrg_color_t *out);
/*
 * primaries from xy chromaticities {rx, ry, gx, gy, bx, by, wx, wy}; false
 * (out untouched) when they are unusable: a chromaticity outside x, y >= 0,
 * x + y <= 1 (y > 0), primaries on one line, a white outside them
 */
extern bool hdrg_color_set_xy(const double xy[8], hdrg_color_t *out);
/*
 * primaries from ICC colorants (rXYZ, gXYZ, bXYZ columns): the white they sum
 * to (D50 in the PCS) is adapted to D65. false when they are unusable
 * (singular, non-finite, a white far from daylight)
 */
extern bool hdrg_color_set_colorants(const double colorants[9], hdrg_color_t *out);
extern void hdrg_curve_set_gamma(double g, hdrg_curve_t *out);
extern void hdrg_curve_set_srgb(hdrg_curve_t *out);
extern double hdrg_curve_eval(const hdrg_curve_t *curve, double x);
/*
 * the color a filter is told of (nego_input_color): the primaries and white
 * as CIE xy {rx, ry, gx, gy, bx, by, wx, wy} and the transfer code (2 = a
 * curve of its own)
 */
extern void hdrg_color_describe(const hdrg_color_t *color, double xy[8], uint32_t *transfer);
/* inverse of a (non-decreasing) curve: linear light in [0, 1] -> code value */
extern double hdrg_curve_inverse(const hdrg_curve_t *curve, double y);
/*
 * Primaries within tolerance of a standard set (BT.709, P3-D65, Adobe RGB,
 * BT.2020) take its exact matrix, curves matching IEC 61966-2-1 become the
 * sRGB curve: profiles of the common spaces give the same result as their
 * codes
 */
extern void hdrg_color_snap(const hdrg_allocator_t *alloc, hdrg_color_t *color);
/* HDRG_SDR_EOTF_*: how the sRGB curve of a source tagged with it is decoded */
extern void hdrg_color_set_sdr_eotf(hdrg_color_t *color, uint8_t sdr_eotf);
extern hdrg_status_t hdrg_color_copy(const hdrg_allocator_t *alloc, const hdrg_color_t *src,
                                     hdrg_color_t *dst);
extern void hdrg_color_free(const hdrg_allocator_t *alloc, hdrg_color_t *color);

/* relative luminance coefficients (the Y row of to_xyz) */
extern void hdrg_color_luma(const hdrg_color_t *color, float coeffs[3]);
/* the source primaries lie within the gamut (tolerance 1e-3) */
extern bool hdrg_color_within(const hdrg_color_t *color, uint8_t gamut);
/* the matrix (rows) from color's linear RGB into a gamut's, D65 white to white */
extern void hdrg_color_to_gamut(const hdrg_color_t *color, uint8_t gamut, float out[9]);
/* luminance coefficients of a target gamut */
extern void hdrg_gamut_luma(uint8_t gamut, float coeffs[3]);
/*
 * The mapping of params (expand_*, saturation, the preset's shape) for an
 * output gamut: toward it, or toward Display P3 written in BT.2020 for a
 * BT.2020 container
 */
extern void hdrg_gamut_map_init(const hdrg_params_t *params, uint8_t target, hdrg_gamut_map_t *out);

/*
 * max_code: 255 or 65535; eotf: the source's code -> linear tables of src,
 * max_code + 1 entries per channel, kept by the caller while the LUT is used
 */
extern hdrg_status_t hdrg_color_lut_build(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                          const hdrg_color_t *src, const float *eotf,
                                          uint32_t max_code, const hdrg_gamut_map_t *map,
                                          hdrg_lut_t *out);
extern void hdrg_color_lut_free(const hdrg_allocator_t *alloc, hdrg_lut_t *lut);

/* Oklab of D65-relative XYZ, and back (exposed for the tests) */
extern void hdrg_oklab_from_xyz(const double xyz[3], double lab[3]);
extern void hdrg_oklab_to_xyz(const double lab[3], double xyz[3]);

#endif /* HDRG_CORE_COLOR_H */
