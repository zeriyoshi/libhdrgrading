/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * A port of AMD FidelityFX Super Resolution 1.0.2 (EASU and RCAS,
 * third_party/fidelityfx-fsr/ffx-fsr/ffx_fsr1.h):
 * Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HDRG_PLUGINS_FSR_H
#define HDRG_PLUGINS_FSR_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * FSR 1 (hdrg.scale.fsr1 and its GPU versions): EASU, the edge-adaptive
 * spatial upscale of 12 taps, then RCAS, the robust contrast-adaptive
 * sharpening, on [0, 1] code values. The 32-bit float operations of
 * ffx_fsr1.h's FsrEasuF and FsrRcasF in their order, with its bit-trick
 * reciprocals; the edges clamp. Unlike FSR, a division by 0 (a black or a
 * white neighbourhood in RCAS, no weight in EASU) gives no NaN: that
 * neighbourhood keeps its sample; and RCAS divides by its weights exactly
 * (FSR's approximate reciprocal moves a flat area by up to 0.1 %)
 */
#define HDRG_FSR_RCAS_LIMIT (0.25f - 1.0f / 16.0f)
#define HDRG_FSR_B          0u /* EASU's taps (ffx_fsr1.h's names) */
#define HDRG_FSR_C          1u
#define HDRG_FSR_E          2u
#define HDRG_FSR_F          3u
#define HDRG_FSR_G          4u
#define HDRG_FSR_H          5u
#define HDRG_FSR_I          6u
#define HDRG_FSR_J          7u
#define HDRG_FSR_K          8u
#define HDRG_FSR_L          9u
#define HDRG_FSR_N          10u
#define HDRG_FSR_O          11u

/* EASU's taps around an output pixel: offsets from the sample at or before it */
static const int8_t hdrg_fsr_taps[12][2] = {{0, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {2, 0},
                                            {-1, 1}, {0, 1},  {1, 1},  {2, 1}, {0, 2}, {1, 2}};

static inline float hdrg_fsr_float(uint32_t u)
{
    float f;

    memcpy(&f, &u, sizeof(f));

    return f;
}

static inline uint32_t hdrg_fsr_bits(float f)
{
    uint32_t u;

    memcpy(&u, &f, sizeof(u));

    return u;
}

/* ffx_a.h's APrxLoRcpF1 and APrxLoRsqF1 */
static inline float hdrg_fsr_rcp_lo(float a)
{
    return hdrg_fsr_float(UINT32_C(0x7ef07ebb) - hdrg_fsr_bits(a));
}

static inline float hdrg_fsr_rsq_lo(float a)
{
    return hdrg_fsr_float(UINT32_C(0x5f347d74) - (hdrg_fsr_bits(a) >> 1));
}

static inline float hdrg_fsr_min(float a, float b)
{
    return (b < a) ? b : a;
}

static inline float hdrg_fsr_max(float a, float b)
{
    return (b > a) ? b : a;
}

static inline float hdrg_fsr_abs(float a)
{
    return (a < 0.0f) ? -a : a;
}

static inline float hdrg_fsr_sat(float a)
{
    return hdrg_fsr_min(1.0f, hdrg_fsr_max(0.0f, a));
}

/* FsrEasuCon: output pixel x to its input position x * scale + offset */
static inline void hdrg_fsr_easu_con(uint32_t in, uint32_t out, float *scale, float *offset)
{
    const float rcp = 1.0f / (float)out;

    *scale = (float)in * rcp;
    *offset = 0.5f * (float)in * rcp - 0.5f;
}

/* FsrEasuSetF: one of the four bilinear corners' direction and length */
static inline void hdrg_fsr_easu_set(float *dir_x, float *dir_y, float *len, float w, float la,
                                     float lb, float lc, float ld, float le)
{
    float dc, cb, len_x, dir_x1, ec, ca, len_y, dir_y1;

    dc = ld - lc;
    cb = lc - lb;
    len_x = hdrg_fsr_rcp_lo(hdrg_fsr_max(hdrg_fsr_abs(dc), hdrg_fsr_abs(cb)));
    dir_x1 = ld - lb;
    *dir_x += dir_x1 * w;
    len_x = hdrg_fsr_sat(hdrg_fsr_abs(dir_x1) * len_x);
    len_x *= len_x;
    *len += len_x * w;
    ec = le - lc;
    ca = lc - la;
    len_y = hdrg_fsr_rcp_lo(hdrg_fsr_max(hdrg_fsr_abs(ec), hdrg_fsr_abs(ca)));
    dir_y1 = le - la;
    *dir_y += dir_y1 * w;
    len_y = hdrg_fsr_sat(hdrg_fsr_abs(dir_y1) * len_y);
    len_y *= len_y;
    *len += len_y * w;
}

/* FsrEasuTapF: a tap's weight into the sums */
static inline void hdrg_fsr_easu_tap(float sum[3], float *weight, float off_x, float off_y,
                                     float dir_x, float dir_y, float len_x, float len_y, float lob,
                                     float clp, const float c[3])
{
    float vx, vy, d2, wb, wa, w;

    vx = (off_x * dir_x) + (off_y * dir_y);
    vy = (off_x * -dir_y) + (off_y * dir_x);
    vx *= len_x;
    vy *= len_y;
    d2 = vx * vx + vy * vy;
    d2 = hdrg_fsr_min(d2, clp);
    wb = (2.0f / 5.0f) * d2 + -1.0f;
    wa = lob * d2 + -1.0f;
    wb *= wb;
    wa *= wa;
    wb = (25.0f / 16.0f) * wb + -(25.0f / 16.0f - 1.0f);
    w = wb * wa;
    sum[0] += c[0] * w;
    sum[1] += c[1] * w;
    sum[2] += c[2] * w;
    *weight += w;
}

/*
 * FsrEasuF: the output pixel at position (px, py) past the tap f of the
 * twelve taps (hdrg_fsr_taps' order, R, G, B in [0, 1])
 */
static inline void hdrg_fsr_easu(float taps[12][3], float px, float py, float out[3])
{
    static const uint8_t order[12] = {HDRG_FSR_B, HDRG_FSR_C, HDRG_FSR_I, HDRG_FSR_J,
                                      HDRG_FSR_F, HDRG_FSR_E, HDRG_FSR_K, HDRG_FSR_L,
                                      HDRG_FSR_H, HDRG_FSR_G, HDRG_FSR_O, HDRG_FSR_N};
    uint8_t t, c;
    bool zero;
    float l[12], dir_x, dir_y, len, dir_r, stretch, len_x, len_y, lob, clp, lo[3], hi[3], sum[3],
        weight;

    for (t = 0; t < 12u; t++) {
        l[t] = taps[t][2] * 0.5f + (taps[t][0] * 0.5f + taps[t][1]);
    }

    dir_x = 0.0f;
    dir_y = 0.0f;
    len = 0.0f;
    hdrg_fsr_easu_set(&dir_x, &dir_y, &len, (1.0f - px) * (1.0f - py), l[HDRG_FSR_B], l[HDRG_FSR_E],
                      l[HDRG_FSR_F], l[HDRG_FSR_G], l[HDRG_FSR_J]);
    hdrg_fsr_easu_set(&dir_x, &dir_y, &len, px * (1.0f - py), l[HDRG_FSR_C], l[HDRG_FSR_F],
                      l[HDRG_FSR_G], l[HDRG_FSR_H], l[HDRG_FSR_K]);
    hdrg_fsr_easu_set(&dir_x, &dir_y, &len, (1.0f - px) * py, l[HDRG_FSR_F], l[HDRG_FSR_I],
                      l[HDRG_FSR_J], l[HDRG_FSR_K], l[HDRG_FSR_N]);
    hdrg_fsr_easu_set(&dir_x, &dir_y, &len, px * py, l[HDRG_FSR_G], l[HDRG_FSR_J], l[HDRG_FSR_K],
                      l[HDRG_FSR_L], l[HDRG_FSR_O]);

    /* the direction normalized (none: along x), the kernel stretched along it */
    dir_r = dir_x * dir_x + dir_y * dir_y;
    zero = dir_r < 1.0f / 32768.0f;
    dir_r = zero ? 1.0f : hdrg_fsr_rsq_lo(dir_r);
    dir_x = zero ? 1.0f : dir_x;
    dir_x *= dir_r;
    dir_y *= dir_r;
    len = len * 0.5f;
    len *= len;
    stretch = (dir_x * dir_x + dir_y * dir_y) *
              hdrg_fsr_rcp_lo(hdrg_fsr_max(hdrg_fsr_abs(dir_x), hdrg_fsr_abs(dir_y)));
    len_x = 1.0f + (stretch - 1.0f) * len;
    len_y = 1.0f + -0.5f * len;
    lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
    clp = hdrg_fsr_rcp_lo(lob);

    /* the ring of the four nearest taps bounds the result (no ringing) */
    for (c = 0; c < 3u; c++) {
        lo[c] = hdrg_fsr_min(hdrg_fsr_min(hdrg_fsr_min(taps[HDRG_FSR_F][c], taps[HDRG_FSR_G][c]),
                                          taps[HDRG_FSR_J][c]),
                             taps[HDRG_FSR_K][c]);
        hi[c] = hdrg_fsr_max(hdrg_fsr_max(hdrg_fsr_max(taps[HDRG_FSR_F][c], taps[HDRG_FSR_G][c]),
                                          taps[HDRG_FSR_J][c]),
                             taps[HDRG_FSR_K][c]);
        sum[c] = 0.0f;
    }

    weight = 0.0f;

    for (t = 0; t < 12u; t++) {
        hdrg_fsr_easu_tap(sum, &weight, (float)hdrg_fsr_taps[order[t]][0] - px,
                          (float)hdrg_fsr_taps[order[t]][1] - py, dir_x, dir_y, len_x, len_y, lob,
                          clp, taps[order[t]]);
    }

    for (c = 0; c < 3u; c++) {
        out[c] = (weight != 0.0f)
                     ? hdrg_fsr_min(hi[c], hdrg_fsr_max(lo[c], sum[c] * (1.0f / weight)))
                     : taps[HDRG_FSR_F][c];
    }
}

/*
 * FsrRcasF without its denoise: the sample e sharpened against its four
 * neighbours b (above), d (left), f (right) and h (below), sharpness
 * 2^-stops (FsrRcasCon)
 */
static inline void hdrg_fsr_rcas(const float b[3], const float d[3], const float e[3],
                                 const float f[3], const float h[3], float sharpness, float out[3])
{
    uint8_t c;
    float lo[3], hi[3], hit_min, hit_max, lobe, worst, rcp;

    worst = -HDRG_FSR_RCAS_LIMIT;

    for (c = 0; c < 3u; c++) {
        lo[c] = hdrg_fsr_min(hdrg_fsr_min(hdrg_fsr_min(b[c], d[c]), f[c]), h[c]);
        hi[c] = hdrg_fsr_max(hdrg_fsr_max(hdrg_fsr_max(b[c], d[c]), f[c]), h[c]);
        hit_min = (hi[c] > 0.0f) ? hdrg_fsr_min(lo[c], e[c]) * (1.0f / (4.0f * hi[c])) : 0.0f;
        hit_max = (lo[c] < 1.0f)
                      ? (1.0f - hdrg_fsr_max(hi[c], e[c])) * (1.0f / (4.0f * lo[c] + -4.0f))
                      : 0.0f;
        worst = hdrg_fsr_max(worst, hdrg_fsr_max(-hit_min, hit_max));
    }

    lobe = hdrg_fsr_max(-HDRG_FSR_RCAS_LIMIT, hdrg_fsr_min(worst, 0.0f)) * sharpness;
    rcp = 1.0f / (4.0f * lobe + 1.0f);

    for (c = 0; c < 3u; c++) {
        out[c] = (lobe * b[c] + lobe * d[c] + lobe * h[c] + lobe * f[c] + e[c]) * rcp;
    }
}

#endif /* HDRG_PLUGINS_FSR_H */
