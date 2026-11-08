/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hagc.h"

#include <math.h>
#include <string.h>

#define HDRG_HAGC_HEADROOM_MAX     6.0f
#define HDRG_HAGC_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define HDRG_HAGC_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

static inline uint16_t hdrg_hagc_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* SMPTE ST 2094-50 C.3.7: PCHIP slopes from the (signed) control points */
static float hdrg_hagc_three_point(float h0, float h1, float s0, float s1)
{
    float m;

    m = ((2.0f * h0 + h1) * s0 - h0 * s1) / (h0 + h1);
    if (signbit(m) != signbit(s0)) {
        return 0.0f;
    }

    if (signbit(m) != signbit(s1) && fabsf(m) > 3.0f * fabsf(s0)) {
        return 3.0f * s0;
    }

    return m;
}

static void hdrg_hagc_pchip(hdrg_hagc_curve_t *curve)
{
    const uint8_t n = curve->count;
    uint8_t i;
    float h[HDRG_HAGC_MAX_POINTS], s[HDRG_HAGC_MAX_POINTS];

    for (i = 0; i + 1 < n; i++) {
        h[i] = curve->x[i + 1] - curve->x[i];
        s[i] = (h[i] != 0.0f) ? (curve->y[i + 1] - curve->y[i]) / h[i] : 0.0f;
    }

    for (i = 0; i < n; i++) {
        curve->m[i] = 0.0f;

        if (n == 1) {
            continue;
        }

        if (i == 0) {
            curve->m[i] = (n > 2 && h[0] > 0.0f && h[1] > 0.0f)
                              ? hdrg_hagc_three_point(h[0], h[1], s[0], s[1])
                              : s[0];
        } else if (i + 1 == n) {
            curve->m[i] = (n > 2 && h[i - 1] > 0.0f && h[i - 2] > 0.0f)
                              ? hdrg_hagc_three_point(h[i - 1], h[i - 2], s[i - 1], s[i - 2])
                              : s[i - 1];
        } else if (h[i - 1] > 0.0f && h[i] > 0.0f) {
            if (signbit(s[i - 1]) != signbit(s[i]) || (s[i - 1] == 0.0f && s[i] == 0.0f)) {
                continue;
            }

            curve->m[i] = 3.0f * (h[i - 1] + h[i]) * s[i - 1] * s[i] /
                          ((2.0f * h[i - 1] + h[i]) * s[i - 1] + (h[i - 1] + 2.0f * h[i]) * s[i]);
        }
    }
}

/* Parses an ST 2094-50 payload (any flags), validating like skcms/Skia. */
extern bool hdrg_hagc_parse(const uint8_t *data, size_t size, hdrg_hagc_t *out)
{
    const uint8_t *end = data + size;
    hdrg_hagc_curve_t *curve;
    uint8_t a, i, k, flags, n, common_mix, common_curve, pchip, coeff_flags;
    float sign, sum;

    memset(out, 0, sizeof(*out));
    out->reference_white = 203.0f;

    if (size < 2 || (data[0] & 0x1Fu) != 0) {
        return false;
    }

    if ((data[1] & 0x3Fu) != 0) {
        return false;
    }

    out->has_tone_map = (data[1] & 0x40u) != 0;
    flags = data[1];
    data += 2;

    if ((flags & 0x80u) != 0) {
        if (end - data < 2) {
            return false;
        }

        out->reference_white = (float)hdrg_hagc_u16(data) / 5.0f;
        data += 2;
    }

    if (!out->has_tone_map) {
        return data == end;
    }

    if (end - data < 3) {
        return false;
    }

    out->baseline = (float)hdrg_hagc_u16(data) / 10000.0f;
    flags = data[2];
    data += 3;

    if ((flags & 0x80u) != 0) {
        out->rwtmo = true;

        return (flags & 0x7Fu) == 0;
    }

    out->alt_count = (uint8_t)HDRG_HAGC_MIN((flags >> 4) & 0x07u, HDRG_HAGC_MAX_ALTERNATES);
    out->chromaticities = (flags >> 2) & 0x03u;
    common_mix = (flags >> 1) & 0x01u;
    common_curve = flags & 0x01u;

    if (out->chromaticities == 3) {
        if (end - data < 16) {
            return false;
        }

        data += 16;
    }

    for (a = 0; a < out->alt_count; a++) {
        curve = &out->alt[a];

        if (end - data < 2) {
            return false;
        }

        curve->headroom = (float)hdrg_hagc_u16(data) / 10000.0f;
        data += 2;

        if (a == 0 || common_mix == 0) {
            if (end - data < 1) {
                return false;
            }

            curve->mix_type = data[0] >> 6;
            coeff_flags = data[0] & 0x3Fu;
            data++;

            if (curve->mix_type != 3 && coeff_flags != 0) {
                return false;
            }

            sum = 0.0f;
            for (k = 0; k < 6 && curve->mix_type == 3; k++) {
                if ((coeff_flags & (0x20u >> k)) == 0) {
                    continue;
                }

                if (end - data < 2) {
                    return false;
                }

                sum += (float)hdrg_hagc_u16(data) / 50000.0f;
                data += 2;
            }

            if (curve->mix_type == 3 && sum <= 0.0f) {
                return false;
            }
        } else {
            curve->mix_type = out->alt[0].mix_type;
        }

        if (a == 0 || common_curve == 0) {
            if (end - data < 1 || (data[0] & 0x03u) != 0) {
                return false;
            }

            n = (uint8_t)((data[0] >> 3) + 1u);
            pchip = (data[0] >> 2) & 0x01u;
            data++;

            if (end - data < (ptrdiff_t)n * 2) {
                return false;
            }

            curve->count = n;
            for (i = 0; i < n; i++) {
                curve->x[i] = (float)hdrg_hagc_u16(data + (size_t)i * 2) / 1000.0f;
            }

            data += (size_t)n * 2;
        } else {
            n = out->alt[0].count;
            pchip = out->alt[0].pchip;
            curve->count = n;
            memcpy(curve->x, out->alt[0].x, sizeof(curve->x));
        }

        curve->pchip = pchip;
        if (end - data < (ptrdiff_t)n * (pchip ? 2 : 4)) {
            return false;
        }

        sign = (out->baseline < curve->headroom) ? 1.0f : -1.0f;
        for (i = 0; i < n; i++) {
            curve->y[i] = sign * (float)hdrg_hagc_u16(data + (size_t)i * 2) / 10000.0f;
        }

        data += (size_t)n * 2;

        if (pchip) {
            hdrg_hagc_pchip(curve);
        } else {
            for (i = 0; i < n; i++) {
                curve->m[i] =
                    tanf(((float)HDRG_HAGC_CLAMP(hdrg_hagc_u16(data + (size_t)i * 2), 1u, 35999u) -
                          18000.0f) *
                         (3.14159265f / 36000.0f));
            }

            data += (size_t)n * 2;
        }

        if (curve->headroom == out->baseline ||
            (a > 0 && curve->headroom <= out->alt[a - 1].headroom)) {
            return false;
        }

        for (i = 0; i + 1 < n; i++) {
            if (curve->x[i + 1] < curve->x[i]) {
                return false;
            }
        }
    }

    return data == end && out->baseline <= HDRG_HAGC_HEADROOM_MAX;
}

/* SMPTE ST 2094-50 gain curve (cubic Hermite in x), log2 gain in stops */
extern float hdrg_hagc_eval(const hdrg_hagc_curve_t *curve, float x)
{
    uint8_t i, j;
    float h, t, mi, mj, c3, c2;

    if (curve->count == 0) {
        return 0.0f;
    }

    j = (uint8_t)(curve->count - 1u);
    /* beyond the last point the output level stays constant */
    if (x >= curve->x[j]) {
        return (curve->x[j] > 0.0f) ? curve->y[j] + log2f(curve->x[j] / x) : curve->y[j];
    }

    if (x <= curve->x[0]) {
        return curve->y[0];
    }

    for (i = 0; i + 1 < curve->count && curve->x[i + 1] <= x; i++) {
    }

    j = (uint8_t)(i + 1u);
    h = curve->x[j] - curve->x[i];
    if (h == 0.0f) {
        return curve->y[i];
    }

    mi = curve->m[i] * h;
    mj = curve->m[j] * h;
    c3 = 2.0f * curve->y[i] + mi - 2.0f * curve->y[j] + mj;
    c2 = -3.0f * curve->y[i] + 3.0f * curve->y[j] - 2.0f * mi - mj;
    t = (x - curve->x[i]) / h;

    return ((c3 * t + c2) * t + mi) * t + curve->y[i];
}
