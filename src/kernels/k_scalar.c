/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>

#include "../priv.h"
#include "hdrgrading/foundation/fmath.h"
#include "hdrgrading/foundation/hash.h"
#include "kernels.h"

/* the references every SIMD tier reproduces (the apply pass's stages) */

static inline float hdrg_scalar_luma(const float coeffs[3], const float v[3])
{
    return coeffs[0] * v[0] + coeffs[1] * v[1] + coeffs[2] * v[2];
}

/* smoothstep desaturation of the highlights (f: the position in the shoulder) */
static inline void hdrg_scalar_desat(const float coeffs[3], float strength, float f, float v[3])
{
    uint8_t c;
    float scale, luma;

    f = HDRG_MIN(f, 1.0f);
    f = f * f * (3.0f - 2.0f * f);
    scale = 1.0f - strength * f;
    luma = hdrg_scalar_luma(coeffs, v);

    for (c = 0; c < 3; c++) {
        v[c] = HDRG_MAX(luma + (v[c] - luma) * scale, 0.0f);
    }
}

/* Hermite smoothstep of t in [0, 1] */
static inline float hdrg_scalar_smooth(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

static inline void hdrg_scalar_pack(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                                    uint32_t row_hash, bool dither, bool pq, uint8_t *dst)
{
    uint32_t i, pixel;
    uint8_t c;
    float code;

    for (i = 0; i < n; i++) {
        pixel = hdrg_hash_pixel(row_hash, x0 + i);

        for (c = 0; c < 3; c++) {
            code = (pq ? hdrg_pq_signal(in[c][i]) : hdrg_fmath_srgb_oetf(in[c][i])) * 255.0f;

            if (dither) {
                code += hdrg_hash_tri(hdrg_hash_sample(pixel, c * 2u),
                                      hdrg_hash_sample(pixel, c * 2u + 1u));
            }

            dst[(size_t)i * 3 + c] = (uint8_t)HDRG_CLAMP((int32_t)lrintf(code), 0, 255);
        }
    }
}

static void hdrg_scalar_boost(const hdrg_boost_args_t *args, const float luma[HDRG_KERNEL_BLOCK],
                              const float boost[HDRG_KERNEL_BLOCK],
                              const float lo[HDRG_KERNEL_BLOCK], const float hi[HDRG_KERNEL_BLOCK],
                              float h[HDRG_KERNEL_BLOCK], float log_luma[HDRG_KERNEL_BLOCK])
{
    uint32_t i;
    float l, t;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
        l = hdrg_fmath_log2(HDRG_MAX(luma[i], args->floor));
        t = (l - lo[i]) / HDRG_MAX(hi[i] - lo[i], HDRG_BOOST_SPAN_MIN);
        h[i] = boost[i] * hdrg_scalar_smooth(HDRG_CLAMP(t, 0.0f, 1.0f));
        log_luma[i] = l;
    }
}

static void hdrg_scalar_grade(const hdrg_grade_args_t *args, const float luma[HDRG_KERNEL_BLOCK],
                              const float gain[HDRG_KERNEL_BLOCK],
                              const float base[HDRG_KERNEL_BLOCK], float ratio[HDRG_KERNEL_BLOCK])
{
    uint32_t i;
    float t, graded, d;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
        t = hdrg_fmath_log2(HDRG_MAX(luma[i], args->floor));
        graded = args->pivot + (t - args->pivot) * args->contrast + gain[i];

        if (args->clarity_on) {
            d = t - base[i];
            graded += args->clarity * (d / (1.0f + fabsf(d) / HDRG_KERNEL_CLARITY_RANGE));
        }

        /* shadow floor: never render darker than the SDR original */
        graded = HDRG_MAX(graded, t);
        ratio[i] = hdrg_fmath_exp2(graded - t);
    }
}

static void hdrg_scalar_lut(const hdrg_lut_t *lut, const uint32_t codes[3][HDRG_KERNEL_BLOCK],
                            const float ratio[HDRG_KERNEL_BLOCK], float rgb[3][HDRG_KERNEL_BLOCK])
{
    uint32_t i;
    uint8_t c;
    float v[3];

    for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
        hdrg_lut_rgb(lut, codes[0][i], codes[1][i], codes[2][i], v);

        for (c = 0; c < 3; c++) {
            rgb[c][i] = v[c] * ratio[i];
        }
    }
}

static void hdrg_scalar_layer(const hdrg_layer_args_t *args, const float rgb[3][HDRG_KERNEL_BLOCK],
                              const float guide[HDRG_KERNEL_BLOCK], float out[3][HDRG_KERNEL_BLOCK],
                              float lr[HDRG_KERNEL_BLOCK])
{
    uint32_t i;
    uint8_t c;
    float v[3], peak, decided, scale, rolled;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
        for (c = 0; c < 3; c++) {
            v[c] = rgb[c][i];
        }

        peak = HDRG_MAX(v[0], HDRG_MAX(v[1], v[2]));
        decided = peak * guide[i];

        if (args->desat > 0.0f && decided > args->start) {
            hdrg_scalar_desat(args->coeffs, args->desat, (decided - args->start) / args->range, v);
            peak = HDRG_MAX(v[0], HDRG_MAX(v[1], v[2]));
        }

        /* the C1 exponential shoulder: gradation above the cap survives */
        if (peak > args->start) {
            rolled = args->start +
                     args->range * (1.0f - hdrg_fmath_exp(-(peak - args->start) / args->range));
            scale = rolled / peak;

            for (c = 0; c < 3; c++) {
                v[c] *= scale;
            }

            peak = rolled;
        }

        if (peak <= args->knee || peak <= 0.0f) {
            scale = 1.0f;
            lr[i] = 0.0f;
        } else {
            rolled = args->knee +
                     (1.0f - args->knee) *
                         (1.0f - hdrg_fmath_exp(-(peak - args->knee) / (1.0f - args->knee)));
            scale = rolled / peak;
            lr[i] = hdrg_fmath_log2(peak / rolled);
        }

        for (c = 0; c < 3; c++) {
            out[c][i] = args->hybrid ? v[c] * args->white : HDRG_CLAMP(v[c] * scale, 0.0f, 1.0f);
        }
    }
}

static void hdrg_scalar_shoulder(const hdrg_shoulder_args_t *args,
                                 const float rgb[3][HDRG_KERNEL_BLOCK],
                                 const float guide[HDRG_KERNEL_BLOCK],
                                 float out[3][HDRG_KERNEL_BLOCK])
{
    uint32_t i;
    uint8_t c;
    float v[3], peak, decided, scale;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
        for (c = 0; c < 3; c++) {
            v[c] = rgb[c][i];
        }

        peak = HDRG_MAX(v[0], HDRG_MAX(v[1], v[2])) * args->white;
        decided = peak * guide[i];

        if (args->desat > 0.0f && decided > args->knee) {
            hdrg_scalar_desat(args->coeffs, args->desat, (decided - args->knee) / args->range, v);
            peak = HDRG_MAX(v[0], HDRG_MAX(v[1], v[2])) * args->white;
        }

        if (peak > args->knee) {
            scale = args->knee +
                    args->range * (1.0f - hdrg_fmath_exp(-(peak - args->knee) / args->range));
            scale /= peak;

            for (c = 0; c < 3; c++) {
                v[c] *= scale;
            }
        }

        for (c = 0; c < 3; c++) {
            out[c][i] = v[c] * args->white;
        }
    }
}

static void hdrg_scalar_pack_srgb(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                                  uint32_t row_hash, bool dither, uint8_t *dst)
{
    hdrg_scalar_pack(in, n, x0, row_hash, dither, false, dst);
}

static void hdrg_scalar_pack_pq(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                                uint32_t row_hash, bool dither, uint8_t *dst)
{
    hdrg_scalar_pack(in, n, x0, row_hash, dither, true, dst);
}

static void hdrg_scalar_pack_srgb16(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint16_t *dst)
{
    uint32_t i;
    uint8_t c;

    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            dst[(size_t)i * 3 + c] = (uint16_t)HDRG_CLAMP(
                (int32_t)lrintf(hdrg_fmath_srgb_oetf(in[c][i]) * 65535.0f), 0, 65535);
        }
    }
}

static void hdrg_scalar_pack_pq16(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint16_t *dst)
{
    uint32_t i;
    uint8_t c;

    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            dst[(size_t)i * 3 + c] = (uint16_t)HDRG_CLAMP(
                (int32_t)lrintf(hdrg_pq_signal(in[c][i]) * 65535.0f), 0, 65535);
        }
    }
}

const hdrg_kernels_t hdrg_kernels_scalar = {
    .boost = hdrg_scalar_boost,
    .grade = hdrg_scalar_grade,
    .lut = hdrg_scalar_lut,
    .layer = hdrg_scalar_layer,
    .shoulder = hdrg_scalar_shoulder,
    .pack_srgb = hdrg_scalar_pack_srgb,
    .pack_pq = hdrg_scalar_pack_pq,
    .pack_srgb16 = hdrg_scalar_pack_srgb16,
    .pack_pq16 = hdrg_scalar_pack_pq16,
    .tier = HDRG_SIMD_SCALAR,
    .exact = true,
};
