/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "dynamic.h"

#include <string.h>

#include "hdrgrading/foundation/fpenv.h"
#include "hdrgrading/foundation/math.h"

/* the gain curve: a control point's hdrg_lightstat_t bins (32 points of 5 / 8 octave), samples */
#define HDRG_DYNAMIC_POINT_BINS  5u
#define HDRG_DYNAMIC_POINT_LEAST 8u

/* HDR10+'s percentages of its percentiles (hdrg_hdr10plus_t) */
static const uint8_t hdrg_dynamic_percentages[HDRG_HDR10PLUS_PERCENTILES] = {1,  5,  10, 25, 50,
                                                                             75, 90, 95, 99};

/* the low bits of v at bit *at of out (zeroed), the most significant first */
static inline void hdrg_dynamic_put(uint8_t *out, uint32_t *at, uint8_t bits, uint32_t v)
{
    uint8_t i;

    for (i = bits; i > 0; i--) {
        if (((v >> (i - 1u)) & 1u) != 0) {
            out[*at >> 3] |= (uint8_t)(0x80u >> (*at & 7u));
        }

        (*at)++;
    }
}

static inline void hdrg_dynamic_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* HDR10+'s value of cd/m^2: in 1/100000 of 10000, 0 to 100000 */
static inline uint32_t hdrg_dynamic_hdr10(float nits)
{
    const float v = nits / 10000.0f;

    return (v > 0.0f) ? (uint32_t)((v < 1.0f ? v : 1.0f) * 100000.0f + 0.5f) : 0u;
}

/* ST 2094-50's u16 of v in units of 1 / scale, within 0 and most */
static inline uint16_t hdrg_dynamic_u16(float v, float scale, uint16_t most)
{
    const float q = v * scale + 0.5f;

    return !(q > 0.0f) ? 0u : (q >= (float)most) ? most : (uint16_t)q;
}

extern void hdrg_dynamic_hdr10plus(const hdrg_lightstat_t *light, float avg_nits, float max_nits,
                                   hdrg_hdr10plus_t *out)
{
    uint64_t total, below;
    uint32_t b, p, c;
    double at;

    memset(out, 0, sizeof(*out));
    total = 0;

    for (b = 0; b < HDRG_LIGHT_BINS; b++) {
        total += light->count[b];
    }

    if (total == 0) {
        return;
    }

    for (c = 0; c < 3u; c++) {
        out->maxscl[c] = light->channel[c];
    }

    out->average_maxrgb = avg_nits;

    for (p = 0; p < HDRG_HDR10PLUS_PERCENTILES; p++) {
        at = (double)total * (double)hdrg_dynamic_percentages[p] / 100.0;
        below = 0;

        for (b = 0; b + 1u < HDRG_LIGHT_BINS && (double)(below + light->count[b]) < at; b++) {
            below += light->count[b];
        }

        out->percentiles[p] = HDRG_MIN(
            hdrg_lightstat_lower(b) +
                (hdrg_lightstat_lower(b + 1u) - hdrg_lightstat_lower(b)) *
                    (float)((light->count[b] > 0) ? (at - (double)below) / (double)light->count[b]
                                                  : 0.0),
            max_nits);
    }

    out->valid = true;
}

extern void hdrg_dynamic_gain_curve(const hdrg_lightstat_t *light, float white, uint8_t gamut,
                                    float baseline, hdrg_st2094_50_t *curve)
{
    uint64_t sum, samples;
    uint32_t g, b, n, i, groups[HDRG_ST2094_50_POINTS];
    double centers;
    float prev, level, gains[HDRG_ST2094_50_POINTS], before, after;

    memset(curve, 0, sizeof(*curve));
    curve->baseline_headroom = baseline;
    curve->reference_white = white;
    curve->gamut = gamut;
    n = 0;

    for (g = 0; curve->baseline_headroom > 0.01f && g < HDRG_LIGHT_BINS / HDRG_DYNAMIC_POINT_BINS;
         g++) {
        samples = 0;
        sum = 0;
        centers = 0.0;

        for (b = g * HDRG_DYNAMIC_POINT_BINS; b < (g + 1u) * HDRG_DYNAMIC_POINT_BINS; b++) {
            samples += light->samples[b];
            sum += light->ratio[b];
            centers += (double)light->samples[b] * (double)hdrg_lightstat_lower(b) +
                       ((double)hdrg_lightstat_lower(b + 1u) - (double)hdrg_lightstat_lower(b)) *
                           ((double)light->place[b] + 0.5 * (double)light->samples[b]) / 256.0;
        }

        if (samples < HDRG_DYNAMIC_POINT_LEAST ||
            !(centers / (double)samples / (double)white >= 0.001) ||
            centers / (double)samples / (double)white > 64.0) {
            continue;
        }

        curve->x[n] = (float)(centers / (double)samples / (double)white);
        gains[n] = (float)(((double)sum / (double)samples - (double)HDRG_LIGHT_SPAN) /
                           (double)HDRG_LIGHT_Q);
        groups[n] = g;
        n++;
    }

    if (n == 0) {
        return;
    }

    prev = -1e30f;

    for (i = 0; i < n; i++) {
        before = (i > 0 && groups[i - 1u] + 1u == groups[i]) ? gains[i - 1u] : gains[i];
        after = (i + 1u < n && groups[i] + 1u == groups[i + 1u]) ? gains[i + 1u] : gains[i];
        curve->y[i] = HDRG_MIN(HDRG_MAX(0.25f * (before + 2.0f * gains[i] + after), -6.0f), 0.0f);
        level = HDRG_MAX(hdrg_math_log2f(curve->x[i]) + curve->y[i], prev);
        curve->y[i] = level - hdrg_math_log2f(curve->x[i]);
        prev = level;
    }

    curve->count = (uint8_t)n;
    curve->valid = true;
}

extern size_t hdrg_hdr10plus_t35(const hdrg_hdr10plus_t *hdr10plus, uint8_t *out, size_t size)
{
    hdrg_fpenv_t fpenv;
    uint32_t at;
    uint8_t c;

    if (hdr10plus == NULL || !hdr10plus->valid) {
        return 0;
    }

    if (out == NULL || size < HDRG_HDR10PLUS_T35_BYTES) {
        return HDRG_HDR10PLUS_T35_BYTES;
    }

    hdrg_fpenv_enter(&fpenv);
    memset(out, 0, HDRG_HDR10PLUS_T35_BYTES);
    at = 0;

    /* application_version 1, one window, no targeted display (nor its actual peak) */
    hdrg_dynamic_put(out, &at, 8, 1u);
    hdrg_dynamic_put(out, &at, 2, 1u);
    hdrg_dynamic_put(out, &at, 27, 0u);
    hdrg_dynamic_put(out, &at, 1, 0u);

    for (c = 0; c < 3u; c++) {
        hdrg_dynamic_put(out, &at, 17, hdrg_dynamic_hdr10(hdr10plus->maxscl[c]));
    }

    hdrg_dynamic_put(out, &at, 17, hdrg_dynamic_hdr10(hdr10plus->average_maxrgb));
    hdrg_dynamic_put(out, &at, 4, HDRG_HDR10PLUS_PERCENTILES);

    for (c = 0; c < HDRG_HDR10PLUS_PERCENTILES; c++) {
        hdrg_dynamic_put(out, &at, 7, hdrg_dynamic_percentages[c]);
        hdrg_dynamic_put(out, &at, 17, hdrg_dynamic_hdr10(hdr10plus->percentiles[c]));
    }

    /* no bright pixels' fraction, mastering display's peak, tone mapping nor saturation */
    hdrg_dynamic_put(out, &at, 10, 0u);
    hdrg_dynamic_put(out, &at, 1, 0u);
    hdrg_dynamic_put(out, &at, 1, 0u);
    hdrg_dynamic_put(out, &at, 1, 0u);
    hdrg_fpenv_leave(&fpenv);

    return HDRG_HDR10PLUS_T35_BYTES;
}

extern size_t hdrg_st2094_50_t35(const hdrg_st2094_50_t *st2094_50, uint8_t *out, size_t size)
{
    hdrg_fpenv_t fpenv;
    uint16_t x, prev;
    uint8_t count, i, *p;
    size_t bytes;
    bool custom;

    if (st2094_50 == NULL || !st2094_50->valid || st2094_50->count == 0 ||
        st2094_50->count > HDRG_ST2094_50_POINTS) {
        return 0;
    }

    count = st2094_50->count;
    custom = st2094_50->reference_white < 202.9f || st2094_50->reference_white > 203.1f;
    bytes = 2u + (custom ? 2u : 0u) + 2u + 1u + 4u + (size_t)count * 4u;

    if (out == NULL || size < bytes) {
        return bytes;
    }

    hdrg_fpenv_enter(&fpenv);

    /* application_version 0 (and its minimum); the reference white when custom, a tone map */
    out[0] = 0x00;
    out[1] = (uint8_t)((custom ? 0x80u : 0u) | 0x40u);
    p = out + 2;

    if (custom) {
        hdrg_dynamic_be16(p, hdrg_dynamic_u16(st2094_50->reference_white, 5.0f, 65535u));
        p += 2;
    }

    /*
     * the baseline headroom; one alternate in the gain's space, its own mix and
     * curve: at headroom 0, of max(R, G, B), PCHIP slopes
     */
    hdrg_dynamic_be16(p, hdrg_dynamic_u16(st2094_50->baseline_headroom, 10000.0f, 60000u));
    p[2] = (uint8_t)(0x10u | (uint8_t)(((st2094_50->gamut < 2u) ? st2094_50->gamut : 2u) << 2));
    hdrg_dynamic_be16(p + 3, 0u);
    p[5] = 0x00;
    p[6] = (uint8_t)((uint8_t)((count - 1u) << 3) | 0x04u);
    p += 7;
    prev = 0;

    for (i = 0; i < count; i++) {
        x = hdrg_dynamic_u16(st2094_50->x[i], 1000.0f, 64000u);
        x = (i > 0 && x <= prev) ? (uint16_t)(prev + 1u) : x;
        hdrg_dynamic_be16(p + (size_t)i * 2u, x);
        hdrg_dynamic_be16(p + (size_t)(count + i) * 2u,
                          hdrg_dynamic_u16(-st2094_50->y[i], 10000.0f, 60000u));
        prev = x;
    }

    hdrg_fpenv_leave(&fpenv);

    return bytes;
}
