/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_KERNELS_H
#define HDRG_KERNELS_KERNELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* pixels per kernel call: one block of the apply pass, a row of lanes per channel */
#define HDRG_KERNEL_BLOCK 16u

/* color LUT: nodes per side over the source code cube */
#define HDRG_LUT_SIZE 33u

/* soft limit of the clarity detail term, stops */
#define HDRG_KERNEL_CLARITY_RANGE 1.5f

/* the boost ramp's smallest span, stops (lo to hi) */
#define HDRG_BOOST_SPAN_MIN 0.05f

/*
 * The PQ signal by table (hdrg_pq_table): y = nits / 10000
 * clamped to [2^-45, 1] (below, the signal rounds to 0 at 16 bits); the
 * float bits of y above the low 17 index 64 steps an octave, the low 17
 * interpolate between them (within 0.3 of a 16-bit code of
 * hdrg_math_pq_oetf)
 */
#define HDRG_PQ_TABLE_ENTRIES 2882u
#define HDRG_PQ_TABLE_LOW     0x1p-45f
#define HDRG_PQ_TABLE_SHIFT   17u
#define HDRG_PQ_TABLE_BASE    5248u /* the bits of 2^-45 shifted: (127 - 45) << 6 */
#define HDRG_PQ_TABLE_MASK    0x1FFFFu
#define HDRG_PQ_TABLE_FRAC    0x1p-17f
#define HDRG_PQ_INV_PEAK      1.0e-4f

/*
 * Every kernel computes all HDRG_KERNEL_BLOCK lanes; the caller fills the
 * lanes past a row's end with finite values (zeros) and drops their results.
 */

/*
 * The source code cube mapped to the target: HDRG_LUT_SIZE^3 nodes of target
 * linear RGB, each channel stored as the source code value its own curve
 * would need for it. Interpolating in the source's code domain keeps the
 * identity exact for any source curve (nodes then lie on a plane), so
 * neutrals and colors the mapping leaves alone come back unchanged; the
 * interpolated codes go back to linear light through the source's EOTF
 * table. Built by hdrg_color_lut_build (src/core/color.h).
 */
typedef struct {
    float *nodes;      /* ((r * N) + g) * N + b, 3 code values each */
    const float *eotf; /* the source's per-channel EOTF tables (borrowed), entries each */
    uint32_t entries;  /* max_code + 1 */
    float max_code;    /* 255 or 65535 */
} hdrg_lut_t;

/*
 * The color LUT at the code values of each lane (hdrg_lut_rgb), times ratio:
 * rendition-gamut linear RGB. Lanes past a row's end hold code 0.
 */
typedef void (*hdrg_lut_fn_t)(const hdrg_lut_t *lut, const uint32_t codes[3][HDRG_KERNEL_BLOCK],
                              const float ratio[HDRG_KERNEL_BLOCK],
                              float rgb[3][HDRG_KERNEL_BLOCK]);

typedef struct {
    float pivot; /* log2 of mid-gray: the S-slope's center */
    float floor; /* smallest analysis luminance */
    float contrast, clarity;
    bool clarity_on; /* base holds the plan's clarity base */
} hdrg_grade_args_t;

/*
 * Grading ratio of the analysis luminance (the apply pass's stage 1), with
 * t = log2(max(luma, floor)) and soft(d) = d / (1 + |d| / CLARITY_RANGE):
 *   t' = max(pivot + (t - pivot) * contrast + gain + clarity * soft(t - base), t)
 *   ratio = 2^(t' - t)
 */
typedef void (*hdrg_grade_fn_t)(const hdrg_grade_args_t *args, const float luma[HDRG_KERNEL_BLOCK],
                                const float gain[HDRG_KERNEL_BLOCK],
                                const float base[HDRG_KERNEL_BLOCK],
                                float ratio[HDRG_KERNEL_BLOCK]);

typedef struct {
    float coeffs[3];    /* rendition-gamut luminance */
    float desat;        /* highlight desaturation strength, 0 = off */
    float start, range; /* shoulder onset and span, diffuse-white units */
    float knee;         /* SDR tone-map knee */
    float white;        /* SDR white, nits (the HDR primary of HYBRID) */
    bool hybrid;
} hdrg_layer_args_t;

/*
 * Two-layer rendition of graded linear RGB: highlight
 * desaturation and the C1 exponential shoulder toward start + range, then
 * the max-channel tone map with its log2 ratio lr. out is the SDR base
 * (clamped to [0, 1]) or, for HYBRID, the pre-tone-map rendition in nits.
 * The desaturation is decided on the peak times guide (the noise-cored
 * analysis luminance over the pixel's, so noise does not move it; 1 = the
 * pixel's own peak); the shoulder on the peak itself.
 */
typedef void (*hdrg_layer_fn_t)(const hdrg_layer_args_t *args,
                                const float rgb[3][HDRG_KERNEL_BLOCK],
                                const float guide[HDRG_KERNEL_BLOCK],
                                float out[3][HDRG_KERNEL_BLOCK], float lr[HDRG_KERNEL_BLOCK]);

typedef struct {
    float coeffs[3];   /* rendition-gamut luminance */
    float desat;       /* highlight desaturation strength, 0 = off */
    float knee, range; /* shoulder onset and span, nits */
    float white;       /* SDR white, nits */
} hdrg_shoulder_args_t;

/*
 * single-layer PQ: highlight desaturation (decided on the peak times guide,
 * as in the layer kernel) and the C1 shoulder toward the peak; out in nits
 */
typedef void (*hdrg_shoulder_fn_t)(const hdrg_shoulder_args_t *args,
                                   const float rgb[3][HDRG_KERNEL_BLOCK],
                                   const float guide[HDRG_KERNEL_BLOCK],
                                   float out[3][HDRG_KERNEL_BLOCK]);

typedef struct {
    float floor; /* smallest analysis luminance */
} hdrg_boost_args_t;

/*
 * Highlight boost of each pixel (the plan pass): the share h
 * of the headroom above the diffuse level, the light's boost times the
 * Hermite smoothstep of log2 La from lo (the light's surround) to hi (its
 * core), both log2 La:
 *   l = log2(max(luma, floor))
 *   h = boost * s(clamp((l - lo) / max(hi - lo, HDRG_BOOST_SPAN_MIN), 0, 1))
 * On the log axis the ramp keeps its shape when a fade scales the frame.
 * log_luma receives l (the clarity base's moment).
 */
typedef void (*hdrg_boost_fn_t)(const hdrg_boost_args_t *args, const float luma[HDRG_KERNEL_BLOCK],
                                const float boost[HDRG_KERNEL_BLOCK],
                                const float lo[HDRG_KERNEL_BLOCK],
                                const float hi[HDRG_KERNEL_BLOCK], float h[HDRG_KERNEL_BLOCK],
                                float log_luma[HDRG_KERNEL_BLOCK]);

/*
 * Packs one block of OETF inputs into interleaved 8-bit RGB at dst (the
 * block's first pixel): code = OETF(v) * 255, plus the triangular dither of
 * pixel x0 + i, channel c in the row's hash state (hdrg_hash_row), rounded to
 * nearest even and clamped to [0, 255]; n pixels are written.
 */
typedef void (*hdrg_pack_fn_t)(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                               uint32_t row_hash, bool dither, uint8_t *dst);

/*
 * Packs one block of OETF inputs into interleaved 16-bit codes at dst:
 * OETF(v) * 65535, rounded to nearest even and clamped (no dither at this
 * depth); n pixels are written.
 */
typedef void (*hdrg_pack16_fn_t)(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint16_t *dst);

/*
 * The kernels of one SIMD tier. Each tier runs the scalar
 * reference's operation sequence lane by lane (IEEE operations, the
 * hdrgrading/foundation/fmath.h constants). An exact tier gives the scalar bytes
 * (test_kernels compares them with memcmp); a tier built with HDRG_SIMD_FMA
 * fuses a product and the sum after it, within the L0x tolerance.
 */
typedef struct {
    hdrg_boost_fn_t boost;
    hdrg_grade_fn_t grade;
    hdrg_lut_fn_t lut;
    hdrg_layer_fn_t layer;
    hdrg_shoulder_fn_t shoulder;
    hdrg_pack_fn_t pack_srgb; /* linear light in [0, 1] -> sRGB */
    hdrg_pack_fn_t pack_pq;   /* absolute nits -> SMPTE ST 2084 */
    hdrg_pack16_fn_t pack_srgb16;
    hdrg_pack16_fn_t pack_pq16;
    uint8_t tier; /* HDRG_SIMD_* */
    bool exact;   /* byte-identical to the scalar reference */
} hdrg_kernels_t;

extern const float hdrg_pq_table[HDRG_PQ_TABLE_ENTRIES];

/* the PQ signal of absolute nits (clamped to [0, 10000], NaN as 0) */
static inline float hdrg_pq_signal(float nits)
{
    uint32_t bits, k;
    float y, t;

    y = nits * HDRG_PQ_INV_PEAK;
    y = (y > 1.0f) ? 1.0f : y;
    y = (y >= HDRG_PQ_TABLE_LOW) ? y : HDRG_PQ_TABLE_LOW;
    memcpy(&bits, &y, sizeof(bits));
    k = (bits >> HDRG_PQ_TABLE_SHIFT) - HDRG_PQ_TABLE_BASE;
    t = (float)(bits & HDRG_PQ_TABLE_MASK) * HDRG_PQ_TABLE_FRAC;

    return hdrg_pq_table[k] + t * (hdrg_pq_table[k + 1u] - hdrg_pq_table[k]);
}

/* tetrahedral interpolation of the LUT at code values (r, g, b): target linear RGB */
static inline void hdrg_lut_rgb(const hdrg_lut_t *lut, uint32_t r, uint32_t g, uint32_t b,
                                float out[3])
{
    const uint32_t n = HDRG_LUT_SIZE, sr = n * n * 3u, sg = n * 3u, sb = 3u;
    const float *p0, *p1, *p2, *p3, *table;
    uint32_t ir, ig, ib, i;
    uint8_t c;
    float fr, fg, fb, w0, w1, w2, w3, v;

    /* exact at the nodes: code * (N - 1) is an integer */
    fr = (float)(r * (n - 1u)) / lut->max_code;
    fg = (float)(g * (n - 1u)) / lut->max_code;
    fb = (float)(b * (n - 1u)) / lut->max_code;
    ir = (uint32_t)fr;
    ig = (uint32_t)fg;
    ib = (uint32_t)fb;
    ir = (ir < n - 2u) ? ir : n - 2u;
    ig = (ig < n - 2u) ? ig : n - 2u;
    ib = (ib < n - 2u) ? ib : n - 2u;
    fr -= (float)ir;
    fg -= (float)ig;
    fb -= (float)ib;

    p0 = lut->nodes + (size_t)ir * sr + (size_t)ig * sg + (size_t)ib * sb;
    p3 = p0 + sr + sg + sb;

    /* the six tetrahedra of the cell, split along its main diagonal */
    if (fr >= fg) {
        if (fg >= fb) {
            p1 = p0 + sr;
            p2 = p1 + sg;
            w0 = 1.0f - fr;
            w1 = fr - fg;
            w2 = fg - fb;
            w3 = fb;
        } else if (fr >= fb) {
            p1 = p0 + sr;
            p2 = p1 + sb;
            w0 = 1.0f - fr;
            w1 = fr - fb;
            w2 = fb - fg;
            w3 = fg;
        } else {
            p1 = p0 + sb;
            p2 = p1 + sr;
            w0 = 1.0f - fb;
            w1 = fb - fr;
            w2 = fr - fg;
            w3 = fg;
        }
    } else if (fb >= fg) {
        p1 = p0 + sb;
        p2 = p1 + sg;
        w0 = 1.0f - fb;
        w1 = fb - fg;
        w2 = fg - fr;
        w3 = fr;
    } else if (fb >= fr) {
        p1 = p0 + sg;
        p2 = p1 + sb;
        w0 = 1.0f - fg;
        w1 = fg - fb;
        w2 = fb - fr;
        w3 = fr;
    } else {
        p1 = p0 + sg;
        p2 = p1 + sr;
        w0 = 1.0f - fg;
        w1 = fg - fr;
        w2 = fr - fb;
        w3 = fb;
    }

    for (c = 0; c < 3; c++) {
        v = w0 * p0[c] + w1 * p1[c] + w2 * p2[c] + w3 * p3[c];
        i = (uint32_t)v;
        i = (i < lut->entries - 2u) ? i : lut->entries - 2u;
        v -= (float)i;
        table = lut->eotf + (size_t)c * lut->entries;
        out[c] = table[i] + v * (table[i + 1u] - table[i]);
    }
}

extern const hdrg_kernels_t hdrg_kernels_scalar;
#ifdef HDRG_KERNELS_X86
extern const hdrg_kernels_t hdrg_kernels_sse41, hdrg_kernels_avx2;
#endif
#ifdef HDRG_KERNELS_AVX512
extern const hdrg_kernels_t hdrg_kernels_avx512;
#endif
#ifdef HDRG_KERNELS_NEON
extern const hdrg_kernels_t hdrg_kernels_neon;
#endif
#ifdef HDRG_KERNELS_SVE
extern const hdrg_kernels_t hdrg_kernels_sve;
#endif
#ifdef HDRG_KERNELS_WASM128
extern const hdrg_kernels_t hdrg_kernels_wasm128;
#endif

/* the kernels of tier when this build has them and the CPU runs them, else NULL */
extern const hdrg_kernels_t *hdrg_kernels_get(uint8_t tier);
/*
 * The best tier this CPU runs, at most cap (0 = no cap). A cap of another
 * architecture's tier gives scalar: the caller asked for a common tier. The
 * provisional tiers (AVX-512, SVE: not yet verified on hardware) run only
 * under a cap that names them or a tier above them.
 */
extern const hdrg_kernels_t *hdrg_kernels_select(uint8_t cap);
/* a tier name of HDRG_SIMD_MAX ("scalar", "sse41", "avx2", ...); 0 = NULL or unknown */
extern uint8_t hdrg_kernels_parse(const char *name);

#endif /* HDRG_KERNELS_KERNELS_H */
