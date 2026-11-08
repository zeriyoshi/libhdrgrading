/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_TOOLS_HAGC_H
#define HDRG_TOOLS_HAGC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The reader of an ICC HAGC payload (what hdrg_st2094_50_t35 writes) for hdrg-inspect and the
 * tests: not the library's (it writes the payload alone)
 */
#define HDRG_HAGC_MAX_ALTERNATES 4u
#define HDRG_HAGC_MAX_POINTS     32u

/*
 * Headroom Adaptive Gain Curve (SMPTE ST 2094-50 adaptive tone map; ICC tag
 * 'HAGC' of type 'hagc'). Each alternate maps the HDR signal x (linear,
 * 1.0 = reference white) to a log2 gain for a display headroom of
 * `headroom` stops; viewers interpolate between alternates and the
 * baseline (gain 0) by headroom.
 */
typedef struct {
    float x[HDRG_HAGC_MAX_POINTS], y[HDRG_HAGC_MAX_POINTS], m[HDRG_HAGC_MAX_POINTS];
    float headroom;
    uint8_t count, mix_type, pchip;
} hdrg_hagc_curve_t;

typedef struct {
    hdrg_hagc_curve_t alt[HDRG_HAGC_MAX_ALTERNATES];
    float reference_white, baseline;
    uint8_t alt_count, chromaticities; /* 0 = BT.709, 1 = P3 (EG 432-1), 2 = BT.2020 */
    bool has_tone_map, rwtmo;
} hdrg_hagc_t;

/* the payload of size bytes into out; false: not one */
extern bool hdrg_hagc_parse(const uint8_t *data, size_t size, hdrg_hagc_t *out);
/* the log2 gain of curve at x (1.0 = reference white) */
extern float hdrg_hagc_eval(const hdrg_hagc_curve_t *curve, float x);

#endif /* HDRG_TOOLS_HAGC_H */
