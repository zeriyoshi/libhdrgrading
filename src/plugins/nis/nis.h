/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * A port of NVIDIA Image Scaling 1.0.3 (the scaler of NIS_Scaler.h and
 * NVScalerUpdateConfig of NIS_Config.h, third_party/nis/NIS):
 * The MIT License(MIT)
 * Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files(the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef HDRG_PLUGINS_NIS_H
#define HDRG_PLUGINS_NIS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * NIS (hdrg.scale.nis and its GPU versions): the luma of the source (BT.709
 * weights of the code values, in integers: hdrg_nis_y_codes) filtered by 6 x 6 taps of 64 phases,
 * blended with four directional filters by the edges a 3 x 3 gradient finds, each sharpened
 * (unsharp mask, limited, and less where the contrast differs between its halves: LTI); the
 * difference of that luma from the bilinear source's is added to the bilinear source's R, G and B.
 * Its SDR settings, 32-bit floats in NIS's order; the edges clamp. Upscaling up to 2x
 * (NVScalerUpdateConfig's range). The tables come from NIS_Config.h
 * (generated at configure: nis_coefs.c)
 */
#define HDRG_NIS_PHASES 64u
#define HDRG_NIS_COLS   8u /* the tables' columns (6 used) */

extern const float hdrg_nis_coef_scale[HDRG_NIS_PHASES][HDRG_NIS_COLS],
    hdrg_nis_coef_usm[HDRG_NIS_PHASES][HDRG_NIS_COLS];

/* NVScalerUpdateConfig's constants (NISHDRMode::None) */
typedef struct {
    float detect_ratio, detect_thres, min_contrast_ratio, ratio_norm, contrast_boost, eps;
    float sharp_start_y, sharp_scale_y, sharp_strength_min, sharp_strength_scale;
    float sharp_limit_min, sharp_limit_scale, scale_x, scale_y;
} hdrg_nis_config_t;

static inline float hdrg_nis_min(float a, float b)
{
    return (b < a) ? b : a;
}

static inline float hdrg_nis_max(float a, float b)
{
    return (b > a) ? b : a;
}

static inline float hdrg_nis_abs(float a)
{
    return (a < 0.0f) ? -a : a;
}

static inline float hdrg_nis_sat(float a)
{
    return hdrg_nis_min(1.0f, hdrg_nis_max(0.0f, a));
}

/* HLSL's lerp: a + t (b - a) */
static inline float hdrg_nis_lerp(float a, float b, float t)
{
    return a + t * (b - a);
}

static inline float hdrg_nis_y(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/*
 * the source's luma from its 16-bit codes in integers (the same BT.709
 * weights), one rounding: the CPU and the GPU agree on it exactly, so the
 * edges' decisions do too
 */
static inline float hdrg_nis_y_codes(uint32_t r, uint32_t g, uint32_t b)
{
    return (float)(2126u * r + 7152u * g + 722u * b) * (1.0f / 655350000.0f);
}

/* NVScalerUpdateConfig for sharpness (0 to 1) and the sizes; false out of its range */
static inline bool hdrg_nis_setup(float sharpness, uint32_t in_w, uint32_t in_h, uint32_t out_w,
                                  uint32_t out_h, hdrg_nis_config_t *c)
{
    float slider, max_scale, min_scale, limit_scale, max_contrast, sharp_end, strength_max,
        limit_max;

    sharpness = hdrg_nis_max(hdrg_nis_min(1.0f, sharpness), 0.0f);
    slider = sharpness - 0.5f;
    max_scale = (slider >= 0.0f) ? 1.25f : 1.75f;
    min_scale = (slider >= 0.0f) ? 1.25f : 1.0f;
    limit_scale = (slider >= 0.0f) ? 1.25f : 1.0f;
    c->detect_ratio = 2 * 1127.f / 1024.f;
    c->detect_thres = 64.0f / 1024.0f;
    c->min_contrast_ratio = 2.0f;
    max_contrast = 10.0f;
    c->sharp_start_y = 0.45f;
    sharp_end = 0.9f;
    c->sharp_strength_min = hdrg_nis_max(0.0f, 0.4f + slider * min_scale * 1.2f);
    strength_max = 1.6f + slider * max_scale * 1.8f;
    c->sharp_limit_min = hdrg_nis_max(0.1f, 0.14f + slider * limit_scale * 0.32f);
    limit_max = 0.5f + slider * limit_scale * 0.6f;
    c->ratio_norm = 1.0f / (max_contrast - c->min_contrast_ratio);
    c->sharp_scale_y = 1.0f / (sharp_end - c->sharp_start_y);
    c->sharp_strength_scale = strength_max - c->sharp_strength_min;
    c->sharp_limit_scale = limit_max - c->sharp_limit_min;
    c->contrast_boost = 1.0f;
    c->eps = 1.0f / 255.0f;
    c->scale_x = (float)in_w / (float)out_w;
    c->scale_y = (float)in_h / (float)out_h;

    return c->scale_x >= 0.5f && c->scale_x <= 1.0f && c->scale_y >= 0.5f && c->scale_y <= 1.0f;
}

/* GetEdgeMap: the weights of 0, 90, 45 and 135 degrees of the 3 x 3 luma p */
static inline void hdrg_nis_edge(float p[3][3], const hdrg_nis_config_t *c, float w[4])
{
    bool c_090, c_45135, is_0, is_45;
    float g0, g45, g90, g135, max_090, min_090, max_45135, min_45135, e_090, e_45135, f_090,
        f_45135;

    g0 = hdrg_nis_abs(p[0][0] + p[0][1] + p[0][2] - p[2][0] - p[2][1] - p[2][2]);
    g45 = hdrg_nis_abs(p[1][0] + p[0][0] + p[0][1] - p[2][1] - p[2][2] - p[1][2]);
    g90 = hdrg_nis_abs(p[0][0] + p[1][0] + p[2][0] - p[0][2] - p[1][2] - p[2][2]);
    g135 = hdrg_nis_abs(p[1][0] + p[2][0] + p[2][1] - p[0][1] - p[0][2] - p[1][2]);
    max_090 = hdrg_nis_max(g0, g90);
    min_090 = hdrg_nis_min(g0, g90);
    max_45135 = hdrg_nis_max(g45, g135);
    min_45135 = hdrg_nis_min(g45, g135);
    w[0] = 0.0f;
    w[1] = 0.0f;
    w[2] = 0.0f;
    w[3] = 0.0f;

    if (max_090 + max_45135 == 0.0f) {
        return;
    }

    e_090 = hdrg_nis_min(max_090 / (max_090 + max_45135), 1.0f);
    e_45135 = 1.0f - e_090;
    c_090 = (max_090 > (min_090 * c->detect_ratio)) && (max_090 > c->detect_thres) &&
            (max_090 > min_45135);
    c_45135 = (max_45135 > (min_45135 * c->detect_ratio)) && (max_45135 > c->detect_thres) &&
              (max_45135 > min_090);
    is_0 = max_090 == g0;
    is_45 = max_45135 == g45;
    f_090 = (c_090 && c_45135) ? e_090 : 1.0f;
    f_45135 = (c_090 && c_45135) ? e_45135 : 1.0f;
    w[0] = (c_090 && is_0) ? f_090 : 0.0f;
    w[1] = (c_090 && !is_0) ? f_090 : 0.0f;
    w[2] = (c_45135 && is_45) ? f_45135 : 0.0f;
    w[3] = (c_45135 && !is_45) ? f_45135 : 0.0f;
}

/* CalcLTI */
static inline float hdrg_nis_lti(const float p[6], uint32_t phase, const hdrg_nis_config_t *c)
{
    const bool selector = phase <= HDRG_NIS_PHASES / 2u;
    float sel, a_min, a_max, b_min, b_max, a_cont, b_cont, ratio;

    sel = selector ? p[0] : p[3];
    a_min = hdrg_nis_min(hdrg_nis_min(p[1], p[2]), sel);
    a_max = hdrg_nis_max(hdrg_nis_max(p[1], p[2]), sel);
    sel = selector ? p[2] : p[5];
    b_min = hdrg_nis_min(hdrg_nis_min(p[3], p[4]), sel);
    b_max = hdrg_nis_max(hdrg_nis_max(p[3], p[4]), sel);
    a_cont = a_max - a_min;
    b_cont = b_max - b_min;
    ratio = hdrg_nis_max(a_cont, b_cont) / (hdrg_nis_min(a_cont, b_cont) + c->eps);

    return (1.0f - hdrg_nis_sat((ratio - c->min_contrast_ratio) * c->ratio_norm)) *
           c->contrast_boost;
}

/* EvalPoly6: 6 taps of phase, sharpened */
static inline float hdrg_nis_poly6(const float p[6], uint32_t phase, const hdrg_nis_config_t *c)
{
    uint8_t i;
    float y, usm, scale, strength, limit;

    y = 0.0f;
    usm = 0.0f;

    for (i = 0; i < 6u; i++) {
        y += hdrg_nis_coef_scale[phase][i] * p[i];
    }

    for (i = 0; i < 6u; i++) {
        usm += hdrg_nis_coef_usm[phase][i] * p[i];
    }

    scale = 1.0f - hdrg_nis_sat((y - c->sharp_start_y) * c->sharp_scale_y);
    strength = scale * c->sharp_strength_scale + c->sharp_strength_min;
    usm *= strength;
    limit = (scale * c->sharp_limit_scale + c->sharp_limit_min) * y;
    usm = hdrg_nis_min(limit, hdrg_nis_max(-limit, usm));
    usm *= hdrg_nis_lti(p, phase, c);

    return y + usm;
}

/* FilterNormal: the separable 6 x 6 taps */
static inline float hdrg_nis_normal(float p[6][6], uint32_t phase_x, uint32_t phase_y)
{
    uint8_t i, j;
    float h, v;

    h = 0.0f;

    for (j = 0; j < 6u; j++) {
        v = 0.0f;

        for (i = 0; i < 6u; i++) {
            v += p[i][j] * hdrg_nis_coef_scale[phase_y][i];
        }

        h += v * hdrg_nis_coef_scale[phase_x][j];
    }

    return h;
}

/* AddDirFilters: the directional filters of the weights w */
static inline float hdrg_nis_directional(float p[6][6], float fx, float fy, uint32_t phase_x,
                                         uint32_t phase_y, const float w[4],
                                         const hdrg_nis_config_t *c)
{
    uint8_t i;
    float f, line[6], t[7], b, plus, a0, a1, a2, a3;

    f = 0.0f;

    if (w[0] > 0.0f) {
        for (i = 0; i < 6u; i++) {
            line[i] = hdrg_nis_lerp(p[i][2], p[i][3], fx);
        }

        f += hdrg_nis_poly6(line, phase_y, c) * w[0];
    }

    if (w[1] > 0.0f) {
        for (i = 0; i < 6u; i++) {
            line[i] = hdrg_nis_lerp(p[2][i], p[3][i], fy);
        }

        f += hdrg_nis_poly6(line, phase_x, c) * w[1];
    }

    if (w[2] > 0.0f) {
        b = 0.5f + 0.5f * (fx - fy);
        t[1] = hdrg_nis_lerp(p[2][1], p[1][2], b);
        t[3] = hdrg_nis_lerp(p[3][2], p[2][3], b);
        t[5] = hdrg_nis_lerp(p[4][3], p[3][4], b);
        b = b - 0.5f;
        a0 = (b >= 0.0f) ? p[0][2] : p[2][0];
        a1 = (b >= 0.0f) ? p[1][3] : p[3][1];
        a2 = (b >= 0.0f) ? p[2][4] : p[4][2];
        a3 = (b >= 0.0f) ? p[3][5] : p[5][3];
        t[0] = hdrg_nis_lerp(p[1][1], a0, hdrg_nis_abs(b));
        t[2] = hdrg_nis_lerp(p[2][2], a1, hdrg_nis_abs(b));
        t[4] = hdrg_nis_lerp(p[3][3], a2, hdrg_nis_abs(b));
        t[6] = hdrg_nis_lerp(p[4][4], a3, hdrg_nis_abs(b));
        plus = fx + fy;

        for (i = 0; i < 6u; i++) {
            line[i] = (plus >= 1.0f) ? t[i + 1u] : t[i];
        }

        plus = (plus >= 1.0f) ? plus - 1.0f : plus;
        f += hdrg_nis_poly6(line, (uint32_t)(plus * 64.0f), c) * w[2];
    }

    if (w[3] > 0.0f) {
        b = 0.5f * (fx + fy);
        t[1] = hdrg_nis_lerp(p[3][1], p[4][2], b);
        t[3] = hdrg_nis_lerp(p[2][2], p[3][3], b);
        t[5] = hdrg_nis_lerp(p[1][3], p[2][4], b);
        b = b - 0.5f;
        a0 = (b >= 0.0f) ? p[5][2] : p[3][0];
        a1 = (b >= 0.0f) ? p[4][3] : p[2][1];
        a2 = (b >= 0.0f) ? p[3][4] : p[1][2];
        a3 = (b >= 0.0f) ? p[2][5] : p[0][3];
        t[0] = hdrg_nis_lerp(p[4][1], a0, hdrg_nis_abs(b));
        t[2] = hdrg_nis_lerp(p[3][2], a1, hdrg_nis_abs(b));
        t[4] = hdrg_nis_lerp(p[2][3], a2, hdrg_nis_abs(b));
        t[6] = hdrg_nis_lerp(p[1][4], a3, hdrg_nis_abs(b));
        plus = 1.0f + (fx - fy);

        for (i = 0; i < 6u; i++) {
            line[i] = (plus >= 1.0f) ? t[i + 1u] : t[i];
        }

        plus = (plus >= 1.0f) ? plus - 1.0f : plus;
        f += hdrg_nis_poly6(line, (uint32_t)(plus * 64.0f), c) * w[3];
    }

    return f;
}

#endif /* HDRG_PLUGINS_NIS_H */
