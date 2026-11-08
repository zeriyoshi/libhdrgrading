/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_VMATH_H
#define HDRG_KERNELS_VMATH_H

#include "hdrgrading/foundation/fmath.h"
#include "hdrgrading/foundation/hash.h"
#include "kernels.h"

/*
 * The SIMD kernels, written once over the primitives of a v_<isa>.h that the
 * including translation unit includes first. Each function repeats the
 * scalar reference (k_scalar.c, hdrgrading/foundation/fmath.h) operation by operation: the
 * ternaries are compares and selects, max(a, b) is a > b ? a : b, and the
 * evaluation order is the C expression's. A product followed by a sum is
 * hdrg_vf_fma: two roundings like the scalar code (every tier byte-identical
 * to it), or fused under HDRG_SIMD_FMA (within the L0x tolerance). A branch
 * the scalar code skips is skipped for a group of lanes only when none of
 * them takes it. Vectors live in variables only, never in arrays or structs
 * (SVE vectors are sizeless); a group is HDRG_V_LANES lanes at lane i of a
 * block, which may be a runtime count (SVE). Only static functions.
 */
#ifndef HDRG_V_LANES
#error "include a kernels/v_<isa>.h before vmath.h"
#endif

static inline hdrg_vf_t hdrg_v_horner(hdrg_vf_t p, hdrg_vf_t x, float c)
{
    return hdrg_vf_fma(p, x, hdrg_vf_dup(c));
}

/* a > b ? a : b (HDRG_MAX) */
static inline hdrg_vf_t hdrg_v_max(hdrg_vf_t a, hdrg_vf_t b)
{
    return hdrg_vf_sel(hdrg_vf_gt(a, b), a, b);
}

/* a < b ? a : b (HDRG_MIN) */
static inline hdrg_vf_t hdrg_v_min(hdrg_vf_t a, hdrg_vf_t b)
{
    return hdrg_vf_sel(hdrg_vf_lt(a, b), a, b);
}

static inline hdrg_vf_t hdrg_v_max3(hdrg_vf_t r, hdrg_vf_t g, hdrg_vf_t b)
{
    return hdrg_v_max(r, hdrg_v_max(g, b));
}

/* v < lo ? lo : (v > hi ? hi : v) (HDRG_CLAMP) */
static inline hdrg_vf_t hdrg_v_clamp(hdrg_vf_t v, float lo, float hi)
{
    const hdrg_vf_t l = hdrg_vf_dup(lo), h = hdrg_vf_dup(hi);

    return hdrg_vf_sel(hdrg_vf_lt(v, l), l, hdrg_vf_sel(hdrg_vf_gt(v, h), h, v));
}

/* Hermite smoothstep t * t * (3 - 2 t) */
static inline hdrg_vf_t hdrg_v_smooth(hdrg_vf_t t)
{
    return hdrg_vf_mul(hdrg_vf_mul(t, t),
                       hdrg_vf_sub(hdrg_vf_dup(3.0f), hdrg_vf_mul(hdrg_vf_dup(2.0f), t)));
}

/* c0 r + c1 g + c2 b (hdrg_rgb_y) */
static inline hdrg_vf_t hdrg_v_luma(const float coeffs[3], hdrg_vf_t r, hdrg_vf_t g, hdrg_vf_t b)
{
    return hdrg_vf_fma(
        hdrg_vf_dup(coeffs[2]), b,
        hdrg_vf_fma(hdrg_vf_dup(coeffs[1]), g, hdrg_vf_mul(hdrg_vf_dup(coeffs[0]), r)));
}

/* hdrg_fmath_log2 */
static inline hdrg_vf_t hdrg_v_log2(hdrg_vf_t x)
{
    const hdrg_vu_t bits = hdrg_vf_bits(x);
    hdrg_vu_t e;
    hdrg_vm_t big;
    hdrg_vf_t m, s, s2, p;

    e = hdrg_vu_sub(hdrg_vu_and(HDRG_VU_SHR(bits, 23), hdrg_vu_dup(0xFFu)), hdrg_vu_dup(127u));
    m = hdrg_vu_bits(
        hdrg_vu_or(hdrg_vu_and(bits, hdrg_vu_dup(0x007FFFFFu)), hdrg_vu_dup(0x3F800000u)));

    big = hdrg_vf_gt(m, hdrg_vf_dup(HDRG_FMATH_SQRT2));
    m = hdrg_vf_sel(big, hdrg_vf_mul(m, hdrg_vf_dup(0.5f)), m);
    e = hdrg_vu_sel(big, hdrg_vu_add(e, hdrg_vu_dup(1u)), e);

    s = hdrg_vf_div(hdrg_vf_sub(m, hdrg_vf_dup(1.0f)), hdrg_vf_add(m, hdrg_vf_dup(1.0f)));
    s2 = hdrg_vf_mul(s, s);
    p = hdrg_vf_dup(HDRG_FMATH_LOG2_C9);
    p = hdrg_v_horner(p, s2, HDRG_FMATH_LOG2_C7);
    p = hdrg_v_horner(p, s2, HDRG_FMATH_LOG2_C5);
    p = hdrg_v_horner(p, s2, HDRG_FMATH_LOG2_C3);
    p = hdrg_v_horner(p, s2, HDRG_FMATH_LOG2_C1);

    return hdrg_vf_fma(s, p, hdrg_vf_from_i32(e));
}

/* hdrg_fmath_exp2 */
static inline hdrg_vf_t hdrg_v_exp2(hdrg_vf_t x)
{
    const hdrg_vf_t round = hdrg_vf_dup(HDRG_FMATH_ROUND), lo = hdrg_vf_dup(HDRG_FMATH_EXP2_MIN),
                    hi = hdrg_vf_dup(HDRG_FMATH_EXP2_MAX);
    hdrg_vf_t n, f, p;

    x = hdrg_vf_sel(hdrg_vf_lt(x, lo), lo, x);
    x = hdrg_vf_sel(hdrg_vf_gt(x, hi), hi, x);
    n = hdrg_vf_sub(hdrg_vf_add(x, round), round);
    f = hdrg_vf_sub(x, n);

    p = hdrg_vf_dup(HDRG_FMATH_EXP2_C7);
    p = hdrg_v_horner(p, f, HDRG_FMATH_EXP2_C6);
    p = hdrg_v_horner(p, f, HDRG_FMATH_EXP2_C5);
    p = hdrg_v_horner(p, f, HDRG_FMATH_EXP2_C4);
    p = hdrg_v_horner(p, f, HDRG_FMATH_EXP2_C3);
    p = hdrg_v_horner(p, f, HDRG_FMATH_EXP2_C2);
    p = hdrg_v_horner(p, f, HDRG_FMATH_EXP2_C1);
    p = hdrg_v_horner(p, f, 1.0f);

    return hdrg_vf_mul(
        p, hdrg_vu_bits(HDRG_VU_SHL(hdrg_vu_add(hdrg_vu_trunc(n), hdrg_vu_dup(127u)), 23)));
}

/* hdrg_fmath_exp */
static inline hdrg_vf_t hdrg_v_exp(hdrg_vf_t x)
{
    return hdrg_v_exp2(hdrg_vf_mul(x, hdrg_vf_dup(HDRG_FMATH_LOG2E)));
}

/* hdrg_fmath_srgb_oetf */
static inline hdrg_vf_t hdrg_v_srgb_oetf(hdrg_vf_t v)
{
    hdrg_vf_t lin, pow;

    v = hdrg_v_clamp(v, 0.0f, 1.0f);

    lin = hdrg_vf_mul(v, hdrg_vf_dup(HDRG_FMATH_SRGB_SLOPE));
    pow = hdrg_vf_fma(hdrg_vf_dup(HDRG_FMATH_SRGB_SCALE),
                      hdrg_v_exp2(hdrg_vf_mul(hdrg_v_log2(v), hdrg_vf_dup(HDRG_FMATH_SRGB_GAMMA))),
                      hdrg_vf_dup(-HDRG_FMATH_SRGB_OFFSET));

    return hdrg_vf_sel(hdrg_vf_le(v, hdrg_vf_dup(HDRG_FMATH_SRGB_KNEE)), lin, pow);
}

/* hdrg_pq_signal */
static inline hdrg_vf_t hdrg_v_pq_signal(hdrg_vf_t nits)
{
    const hdrg_vf_t one = hdrg_vf_dup(1.0f), low = hdrg_vf_dup(HDRG_PQ_TABLE_LOW);
    hdrg_vu_t bits, k;
    hdrg_vf_t y, t, t0, t1;

    y = hdrg_vf_mul(nits, hdrg_vf_dup(HDRG_PQ_INV_PEAK));
    y = hdrg_vf_sel(hdrg_vf_gt(y, one), one, y);
    y = hdrg_vf_sel(hdrg_vf_ge(y, low), y, low);
    bits = hdrg_vf_bits(y);
    k = hdrg_vu_sub(HDRG_VU_SHR(bits, HDRG_PQ_TABLE_SHIFT), hdrg_vu_dup(HDRG_PQ_TABLE_BASE));
    t = hdrg_vf_mul(hdrg_vf_from_i32(hdrg_vu_and(bits, hdrg_vu_dup(HDRG_PQ_TABLE_MASK))),
                    hdrg_vf_dup(HDRG_PQ_TABLE_FRAC));
    hdrg_vf_gather2(hdrg_pq_table, k, &t0, &t1);

    return hdrg_vf_fma(t, hdrg_vf_sub(t1, t0), t0);
}

/* hdrg_hash_mix32 */
static inline hdrg_vu_t hdrg_v_mix32(hdrg_vu_t h)
{
    h = hdrg_vu_xor(h, HDRG_VU_SHR(h, 16));
    h = hdrg_vu_mul(h, hdrg_vu_dup(UINT32_C(0x7feb352d)));
    h = hdrg_vu_xor(h, HDRG_VU_SHR(h, 15));
    h = hdrg_vu_mul(h, hdrg_vu_dup(UINT32_C(0x846ca68b)));

    return hdrg_vu_xor(h, HDRG_VU_SHR(h, 16));
}

/* hdrg_hash_pixel of the lanes' pixels x .. x + HDRG_V_LANES - 1 */
static inline hdrg_vu_t hdrg_v_pixel(uint32_t row_hash, uint32_t x)
{
    return hdrg_v_mix32(
        hdrg_vu_xor(hdrg_vu_dup(row_hash), hdrg_vu_mul(hdrg_vu_add(hdrg_vu_dup(x), hdrg_vu_index()),
                                                       hdrg_vu_dup(HDRG_HASH_W_X))));
}

/* hdrg_hash_tri of the samples 2c and 2c + 1 of each lane's pixel state */
static inline hdrg_vf_t hdrg_v_dither(hdrg_vu_t pixel, uint32_t c)
{
    hdrg_vu_t a, b;

    a = hdrg_v_mix32(hdrg_vu_xor(pixel, hdrg_vu_dup(c * 2u * HDRG_HASH_W_S)));
    b = hdrg_v_mix32(hdrg_vu_xor(pixel, hdrg_vu_dup((c * 2u + 1u) * HDRG_HASH_W_S)));

    return hdrg_vf_mul(
        hdrg_vf_sub(hdrg_vf_from_i32(HDRG_VU_SHR(a, 8)), hdrg_vf_from_i32(HDRG_VU_SHR(b, 8))),
        hdrg_vf_dup(0x1p-24f));
}

/* the C1 exponential shoulder above start: start + range * (1 - e^(-(x - start) / range)) */
static inline hdrg_vf_t hdrg_v_shoulder_of(hdrg_vf_t x, hdrg_vf_t start, hdrg_vf_t range)
{
    return hdrg_vf_fma(
        range,
        hdrg_vf_sub(hdrg_vf_dup(1.0f),
                    hdrg_v_exp(hdrg_vf_div(hdrg_vf_neg(hdrg_vf_sub(x, start)), range))),
        start);
}

/* the smoothstep desaturation of the highlights: lanes in m only */
static inline void hdrg_v_desat(const float coeffs[3], float strength, hdrg_vm_t m, hdrg_vf_t f,
                                hdrg_vf_t *r, hdrg_vf_t *g, hdrg_vf_t *b)
{
    const hdrg_vf_t zero = hdrg_vf_dup(0.0f);
    hdrg_vf_t scale, luma;

    f = hdrg_v_smooth(hdrg_v_min(f, hdrg_vf_dup(1.0f)));
    scale = hdrg_vf_sub(hdrg_vf_dup(1.0f), hdrg_vf_mul(hdrg_vf_dup(strength), f));
    luma = hdrg_v_luma(coeffs, *r, *g, *b);

    *r = hdrg_vf_sel(m, hdrg_v_max(hdrg_vf_fma(hdrg_vf_sub(*r, luma), scale, luma), zero), *r);
    *g = hdrg_vf_sel(m, hdrg_v_max(hdrg_vf_fma(hdrg_vf_sub(*g, luma), scale, luma), zero), *g);
    *b = hdrg_vf_sel(m, hdrg_v_max(hdrg_vf_fma(hdrg_vf_sub(*b, luma), scale, luma), zero), *b);
}

/*
 * The pack kernels' code planes: OETF, x 255 plus the dither, rounded to
 * nearest even and clamped to [0, 255]
 */
static inline void hdrg_v_planes(const float in[3][HDRG_KERNEL_BLOCK], uint32_t x0,
                                 uint32_t row_hash, bool dither, bool pq,
                                 uint8_t planes[3][HDRG_KERNEL_BLOCK])
{
    hdrg_vu_t pixel;
    hdrg_vf_t code;
    uint32_t i;
    uint8_t c;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i += HDRG_V_LANES) {
        pixel = hdrg_v_pixel(row_hash, x0 + i);

        for (c = 0; c < 3; c++) {
            code = hdrg_vf_load_at(in[c], i);
            code = pq ? hdrg_v_pq_signal(code) : hdrg_v_srgb_oetf(code);
            code = dither ? hdrg_vf_fma(code, hdrg_vf_dup(255.0f), hdrg_v_dither(pixel, c))
                          : hdrg_vf_mul(code, hdrg_vf_dup(255.0f));
            hdrg_vu_store_u8_at(planes[c], i, hdrg_vu_round(code));
        }
    }
}

/* w0 a + w1 b + w2 c + w3 d, summed in that order (hdrg_lut_rgb) */
static inline hdrg_vf_t hdrg_v_tetra(hdrg_vf_t w0, hdrg_vf_t w1, hdrg_vf_t w2, hdrg_vf_t w3,
                                     hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c, hdrg_vf_t d)
{
    return hdrg_vf_fma(w3, d, hdrg_vf_fma(w2, c, hdrg_vf_fma(w1, b, hdrg_vf_mul(w0, a))));
}

/* the interpolated code value v of channel c through its EOTF table, times ratio */
static inline hdrg_vf_t hdrg_v_lut_decode(const hdrg_lut_t *lut, uint8_t c, hdrg_vf_t v,
                                          hdrg_vf_t ratio)
{
    hdrg_vu_t k;
    hdrg_vf_t q, t0, t1;

    k = hdrg_vu_min(hdrg_vu_trunc(v), hdrg_vu_dup(lut->entries - 2u));
    q = hdrg_vf_sub(v, hdrg_vf_from_i32(k));
    hdrg_vf_gather2(lut->eotf + (size_t)c * lut->entries, k, &t0, &t1);

    return hdrg_vf_mul(hdrg_vf_fma(q, hdrg_vf_sub(t1, t0), t0), ratio);
}

/* the 16-bit pack kernels: OETF, x 65535, rounded to nearest even and clamped, interleaved */
static inline void hdrg_v_pack16(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, bool pq,
                                 uint16_t *dst)
{
    hdrg_vf_t code;
    uint32_t i;
    uint16_t planes[3][HDRG_KERNEL_BLOCK];
    uint8_t c;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i += HDRG_V_LANES) {
        for (c = 0; c < 3; c++) {
            code = hdrg_vf_load_at(in[c], i);
            code = pq ? hdrg_v_pq_signal(code) : hdrg_v_srgb_oetf(code);
            hdrg_vu_store_u16_at(planes[c], i,
                                 hdrg_vu_round(hdrg_vf_mul(code, hdrg_vf_dup(65535.0f))));
        }
    }

    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            dst[(size_t)i * 3u + c] = planes[c][i];
        }
    }
}

static void hdrg_v_pack_srgb16(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint16_t *dst)
{
    hdrg_v_pack16(in, n, false, dst);
}

static void hdrg_v_pack_pq16(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint16_t *dst)
{
    hdrg_v_pack16(in, n, true, dst);
}

static void hdrg_v_boost(const hdrg_boost_args_t *args, const float luma[HDRG_KERNEL_BLOCK],
                         const float boost[HDRG_KERNEL_BLOCK], const float lo[HDRG_KERNEL_BLOCK],
                         const float hi[HDRG_KERNEL_BLOCK], float h[HDRG_KERNEL_BLOCK],
                         float log_luma[HDRG_KERNEL_BLOCK])
{
    hdrg_vf_t l, low, t;
    uint32_t i;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i += HDRG_V_LANES) {
        l = hdrg_v_log2(hdrg_v_max(hdrg_vf_load_at(luma, i), hdrg_vf_dup(args->floor)));
        low = hdrg_vf_load_at(lo, i);
        t = hdrg_vf_div(hdrg_vf_sub(l, low), hdrg_v_max(hdrg_vf_sub(hdrg_vf_load_at(hi, i), low),
                                                        hdrg_vf_dup(HDRG_BOOST_SPAN_MIN)));
        hdrg_vf_store_at(
            h, i,
            hdrg_vf_mul(hdrg_vf_load_at(boost, i), hdrg_v_smooth(hdrg_v_clamp(t, 0.0f, 1.0f))));
        hdrg_vf_store_at(log_luma, i, l);
    }
}

/*
 * The tetrahedral interpolation of hdrg_lut_rgb for the lanes' code values:
 * the six cases of the cell as masks, the corners' code values gathered
 * (three adjacent floats each), the interpolated code value decoded through
 * the EOTF table (two adjacent entries), times ratio
 */
static void hdrg_v_lut(const hdrg_lut_t *lut, const uint32_t codes[3][HDRG_KERNEL_BLOCK],
                       const float ratio[HDRG_KERNEL_BLOCK], float rgb[3][HDRG_KERNEL_BLOCK])
{
    const uint32_t n = HDRG_LUT_SIZE, sr = n * n * 3u, sg = n * 3u, sb = 3u;
    hdrg_vu_t ir, ig, ib, p0, p1, p2, p3;
    hdrg_vm_t rg, gb, rb, bg, br, c1, c2, c3, c4, c5;
    hdrg_vf_t fr, fg, fb, w0, w1, w2, w3, r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3, scale;
    uint32_t i;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i += HDRG_V_LANES) {
        /* exact at the nodes: code * (N - 1) is an integer */
        fr = hdrg_vf_div(
            hdrg_vf_from_i32(hdrg_vu_mul(hdrg_vu_load_at(codes[0], i), hdrg_vu_dup(n - 1u))),
            hdrg_vf_dup(lut->max_code));
        fg = hdrg_vf_div(
            hdrg_vf_from_i32(hdrg_vu_mul(hdrg_vu_load_at(codes[1], i), hdrg_vu_dup(n - 1u))),
            hdrg_vf_dup(lut->max_code));
        fb = hdrg_vf_div(
            hdrg_vf_from_i32(hdrg_vu_mul(hdrg_vu_load_at(codes[2], i), hdrg_vu_dup(n - 1u))),
            hdrg_vf_dup(lut->max_code));
        ir = hdrg_vu_min(hdrg_vu_trunc(fr), hdrg_vu_dup(n - 2u));
        ig = hdrg_vu_min(hdrg_vu_trunc(fg), hdrg_vu_dup(n - 2u));
        ib = hdrg_vu_min(hdrg_vu_trunc(fb), hdrg_vu_dup(n - 2u));
        fr = hdrg_vf_sub(fr, hdrg_vf_from_i32(ir));
        fg = hdrg_vf_sub(fg, hdrg_vf_from_i32(ig));
        fb = hdrg_vf_sub(fb, hdrg_vf_from_i32(ib));

        /* the six tetrahedra, as the scalar branches choose them */
        rg = hdrg_vf_ge(fr, fg);
        gb = hdrg_vf_ge(fg, fb);
        rb = hdrg_vf_ge(fr, fb);
        bg = hdrg_vf_ge(fb, fg);
        br = hdrg_vf_ge(fb, fr);
        c1 = hdrg_vm_and(rg, gb);
        c2 = hdrg_vm_and(hdrg_vm_and(rg, hdrg_vm_not(gb)), rb);
        c3 = hdrg_vm_and(rg, hdrg_vm_not(hdrg_vm_or(gb, rb)));
        c4 = hdrg_vm_and(hdrg_vm_not(rg), bg);
        c5 = hdrg_vm_and(hdrg_vm_not(hdrg_vm_or(rg, bg)), br);

        w0 = hdrg_vf_sel(hdrg_vm_or(c1, c2), hdrg_vf_sub(hdrg_vf_dup(1.0f), fr),
                         hdrg_vf_sel(hdrg_vm_or(c3, c4), hdrg_vf_sub(hdrg_vf_dup(1.0f), fb),
                                     hdrg_vf_sub(hdrg_vf_dup(1.0f), fg)));
        w1 = hdrg_vf_sel(c1, hdrg_vf_sub(fr, fg),
                         hdrg_vf_sel(c2, hdrg_vf_sub(fr, fb),
                                     hdrg_vf_sel(c3, hdrg_vf_sub(fb, fr),
                                                 hdrg_vf_sel(c4, hdrg_vf_sub(fb, fg),
                                                             hdrg_vf_sel(c5, hdrg_vf_sub(fg, fb),
                                                                         hdrg_vf_sub(fg, fr))))));
        w2 = hdrg_vf_sel(c1, hdrg_vf_sub(fg, fb),
                         hdrg_vf_sel(c2, hdrg_vf_sub(fb, fg),
                                     hdrg_vf_sel(c3, hdrg_vf_sub(fr, fg),
                                                 hdrg_vf_sel(c4, hdrg_vf_sub(fg, fr),
                                                             hdrg_vf_sel(c5, hdrg_vf_sub(fb, fr),
                                                                         hdrg_vf_sub(fr, fb))))));
        w3 = hdrg_vf_sel(
            c1, fb, hdrg_vf_sel(hdrg_vm_or(c2, c3), fg, hdrg_vf_sel(hdrg_vm_or(c4, c5), fr, fb)));

        p0 = hdrg_vu_add(
            hdrg_vu_add(hdrg_vu_mul(ir, hdrg_vu_dup(sr)), hdrg_vu_mul(ig, hdrg_vu_dup(sg))),
            hdrg_vu_mul(ib, hdrg_vu_dup(sb)));
        p1 = hdrg_vu_add(
            p0, hdrg_vu_sel(hdrg_vm_or(c1, c2), hdrg_vu_dup(sr),
                            hdrg_vu_sel(hdrg_vm_or(c3, c4), hdrg_vu_dup(sb), hdrg_vu_dup(sg))));
        p2 = hdrg_vu_add(
            p0,
            hdrg_vu_sel(c1, hdrg_vu_dup(sr + sg),
                        hdrg_vu_sel(c2, hdrg_vu_dup(sr + sb),
                                    hdrg_vu_sel(c3, hdrg_vu_dup(sb + sr),
                                                hdrg_vu_sel(c4, hdrg_vu_dup(sb + sg),
                                                            hdrg_vu_sel(c5, hdrg_vu_dup(sg + sb),
                                                                        hdrg_vu_dup(sg + sr)))))));
        p3 = hdrg_vu_add(p0, hdrg_vu_dup(sr + sg + sb));

        hdrg_vf_gather3(lut->nodes, p0, &r0, &g0, &b0);
        hdrg_vf_gather3(lut->nodes, p1, &r1, &g1, &b1);
        hdrg_vf_gather3(lut->nodes, p2, &r2, &g2, &b2);
        hdrg_vf_gather3(lut->nodes, p3, &r3, &g3, &b3);
        scale = hdrg_vf_load_at(ratio, i);

        hdrg_vf_store_at(
            rgb[0], i,
            hdrg_v_lut_decode(lut, 0, hdrg_v_tetra(w0, w1, w2, w3, r0, r1, r2, r3), scale));
        hdrg_vf_store_at(
            rgb[1], i,
            hdrg_v_lut_decode(lut, 1, hdrg_v_tetra(w0, w1, w2, w3, g0, g1, g2, g3), scale));
        hdrg_vf_store_at(
            rgb[2], i,
            hdrg_v_lut_decode(lut, 2, hdrg_v_tetra(w0, w1, w2, w3, b0, b1, b2, b3), scale));
    }
}

static void hdrg_v_grade(const hdrg_grade_args_t *args, const float luma[HDRG_KERNEL_BLOCK],
                         const float gain[HDRG_KERNEL_BLOCK], const float base[HDRG_KERNEL_BLOCK],
                         float ratio[HDRG_KERNEL_BLOCK])
{
    const hdrg_vf_t pivot = hdrg_vf_dup(args->pivot), contrast = hdrg_vf_dup(args->contrast);
    hdrg_vf_t t, graded, d;
    uint32_t i;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i += HDRG_V_LANES) {
        t = hdrg_v_log2(hdrg_v_max(hdrg_vf_load_at(luma, i), hdrg_vf_dup(args->floor)));
        graded = hdrg_vf_add(hdrg_vf_fma(hdrg_vf_sub(t, pivot), contrast, pivot),
                             hdrg_vf_load_at(gain, i));

        if (args->clarity_on) {
            d = hdrg_vf_sub(t, hdrg_vf_load_at(base, i));
            graded = hdrg_vf_fma(
                hdrg_vf_dup(args->clarity),
                hdrg_vf_div(d, hdrg_vf_add(hdrg_vf_dup(1.0f),
                                           hdrg_vf_div(hdrg_vf_abs(d),
                                                       hdrg_vf_dup(HDRG_KERNEL_CLARITY_RANGE)))),
                graded);
        }

        graded = hdrg_v_max(graded, t);
        hdrg_vf_store_at(ratio, i, hdrg_v_exp2(hdrg_vf_sub(graded, t)));
    }
}

static void hdrg_v_layer(const hdrg_layer_args_t *args, const float rgb[3][HDRG_KERNEL_BLOCK],
                         const float guide[HDRG_KERNEL_BLOCK], float out[3][HDRG_KERNEL_BLOCK],
                         float lr[HDRG_KERNEL_BLOCK])
{
    const hdrg_vf_t one = hdrg_vf_dup(1.0f), zero = hdrg_vf_dup(0.0f),
                    start = hdrg_vf_dup(args->start), range = hdrg_vf_dup(args->range),
                    knee = hdrg_vf_dup(args->knee), span = hdrg_vf_dup(1.0f - args->knee),
                    white = hdrg_vf_dup(args->white);
    hdrg_vm_t m;
    hdrg_vf_t r, g, b, peak, decided, scale, rolled;
    uint32_t i;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i += HDRG_V_LANES) {
        r = hdrg_vf_load_at(rgb[0], i);
        g = hdrg_vf_load_at(rgb[1], i);
        b = hdrg_vf_load_at(rgb[2], i);
        peak = hdrg_v_max3(r, g, b);
        decided = hdrg_vf_mul(peak, hdrg_vf_load_at(guide, i));
        m = hdrg_vf_gt(decided, start);

        if (args->desat > 0.0f && hdrg_vm_any(m)) {
            hdrg_v_desat(args->coeffs, args->desat, m,
                         hdrg_vf_div(hdrg_vf_sub(decided, start), range), &r, &g, &b);
            peak = hdrg_vf_sel(m, hdrg_v_max3(r, g, b), peak);
        }

        m = hdrg_vf_gt(peak, start);

        if (hdrg_vm_any(m)) {
            rolled = hdrg_v_shoulder_of(peak, start, range);
            scale = hdrg_vf_div(rolled, peak);
            r = hdrg_vf_sel(m, hdrg_vf_mul(r, scale), r);
            g = hdrg_vf_sel(m, hdrg_vf_mul(g, scale), g);
            b = hdrg_vf_sel(m, hdrg_vf_mul(b, scale), b);
            peak = hdrg_vf_sel(m, rolled, peak);
        }

        /* the SDR tone map: identity up to the knee */
        m = hdrg_vm_or(hdrg_vf_le(peak, knee), hdrg_vf_le(peak, zero));
        scale = one;
        hdrg_vf_store_at(lr, i, zero);

        if (!hdrg_vm_all(m)) {
            rolled = hdrg_v_shoulder_of(peak, knee, span);
            scale = hdrg_vf_sel(m, one, hdrg_vf_div(rolled, peak));
            hdrg_vf_store_at(lr, i, hdrg_vf_sel(m, zero, hdrg_v_log2(hdrg_vf_div(peak, rolled))));
        }

        if (args->hybrid) {
            hdrg_vf_store_at(out[0], i, hdrg_vf_mul(r, white));
            hdrg_vf_store_at(out[1], i, hdrg_vf_mul(g, white));
            hdrg_vf_store_at(out[2], i, hdrg_vf_mul(b, white));
        } else {
            hdrg_vf_store_at(out[0], i, hdrg_v_clamp(hdrg_vf_mul(r, scale), 0.0f, 1.0f));
            hdrg_vf_store_at(out[1], i, hdrg_v_clamp(hdrg_vf_mul(g, scale), 0.0f, 1.0f));
            hdrg_vf_store_at(out[2], i, hdrg_v_clamp(hdrg_vf_mul(b, scale), 0.0f, 1.0f));
        }
    }
}

static void hdrg_v_shoulder(const hdrg_shoulder_args_t *args, const float rgb[3][HDRG_KERNEL_BLOCK],
                            const float guide[HDRG_KERNEL_BLOCK], float out[3][HDRG_KERNEL_BLOCK])
{
    const hdrg_vf_t knee = hdrg_vf_dup(args->knee), range = hdrg_vf_dup(args->range),
                    white = hdrg_vf_dup(args->white);
    hdrg_vm_t m;
    hdrg_vf_t r, g, b, peak, decided, scale;
    uint32_t i;

    for (i = 0; i < HDRG_KERNEL_BLOCK; i += HDRG_V_LANES) {
        r = hdrg_vf_load_at(rgb[0], i);
        g = hdrg_vf_load_at(rgb[1], i);
        b = hdrg_vf_load_at(rgb[2], i);
        peak = hdrg_vf_mul(hdrg_v_max3(r, g, b), white);
        decided = hdrg_vf_mul(peak, hdrg_vf_load_at(guide, i));
        m = hdrg_vf_gt(decided, knee);

        if (args->desat > 0.0f && hdrg_vm_any(m)) {
            hdrg_v_desat(args->coeffs, args->desat, m,
                         hdrg_vf_div(hdrg_vf_sub(decided, knee), range), &r, &g, &b);
            peak = hdrg_vf_sel(m, hdrg_vf_mul(hdrg_v_max3(r, g, b), white), peak);
        }

        m = hdrg_vf_gt(peak, knee);

        if (hdrg_vm_any(m)) {
            scale = hdrg_vf_div(hdrg_v_shoulder_of(peak, knee, range), peak);
            r = hdrg_vf_sel(m, hdrg_vf_mul(r, scale), r);
            g = hdrg_vf_sel(m, hdrg_vf_mul(g, scale), g);
            b = hdrg_vf_sel(m, hdrg_vf_mul(b, scale), b);
        }

        hdrg_vf_store_at(out[0], i, hdrg_vf_mul(r, white));
        hdrg_vf_store_at(out[1], i, hdrg_vf_mul(g, white));
        hdrg_vf_store_at(out[2], i, hdrg_vf_mul(b, white));
    }
}

#endif /* HDRG_KERNELS_VMATH_H */
