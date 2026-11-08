/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_V_WASM128_H
#define HDRG_KERNELS_V_WASM128_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wasm_simd128.h>

/*
 * The vector primitives vmath.h is written in, for WebAssembly SIMD128 (4
 * lanes; the module is built with -msimd128). SIMD128 has no fused
 * multiply-add (relaxed SIMD is not used: its results depend on the
 * hardware), so the tier is exact under HDRG_SIMD_FMA too. Comparisons are
 * ordered like C's; nearest rounds to even like lrintf in the default mode.
 */
#define HDRG_V_LANES 4u
#define HDRG_V_EXACT true

/* shifts by a count */
#define HDRG_VU_SHR(u, n) wasm_u32x4_shr((u), (n))
#define HDRG_VU_SHL(u, n) wasm_i32x4_shl((u), (n))

typedef v128_t hdrg_vf_t;
typedef v128_t hdrg_vu_t;
typedef v128_t hdrg_vm_t;

static inline hdrg_vf_t hdrg_vf_dup(float c)
{
    return wasm_f32x4_splat(c);
}

static inline hdrg_vf_t hdrg_vf_load_at(const float *p, uint32_t i)
{
    return wasm_v128_load(p + i);
}

static inline void hdrg_vf_store_at(float *p, uint32_t i, hdrg_vf_t v)
{
    wasm_v128_store(p + i, v);
}

static inline hdrg_vf_t hdrg_vf_add(hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_f32x4_add(a, b);
}

static inline hdrg_vf_t hdrg_vf_sub(hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_f32x4_sub(a, b);
}

static inline hdrg_vf_t hdrg_vf_mul(hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_f32x4_mul(a, b);
}

static inline hdrg_vf_t hdrg_vf_div(hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_f32x4_div(a, b);
}

/* a * b + c with the scalar reference's two roundings */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return wasm_f32x4_add(wasm_f32x4_mul(a, b), c);
}

static inline hdrg_vf_t hdrg_vf_neg(hdrg_vf_t a)
{
    return wasm_f32x4_neg(a);
}

static inline hdrg_vf_t hdrg_vf_abs(hdrg_vf_t a)
{
    return wasm_f32x4_abs(a);
}

static inline hdrg_vm_t hdrg_vf_lt(hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_f32x4_lt(a, b);
}

static inline hdrg_vm_t hdrg_vf_gt(hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_f32x4_gt(a, b);
}

static inline hdrg_vm_t hdrg_vf_le(hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_f32x4_le(a, b);
}

static inline hdrg_vm_t hdrg_vf_ge(hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_f32x4_ge(a, b);
}

/* m ? a : b per lane */
static inline hdrg_vf_t hdrg_vf_sel(hdrg_vm_t m, hdrg_vf_t a, hdrg_vf_t b)
{
    return wasm_v128_bitselect(a, b, m);
}

static inline hdrg_vu_t hdrg_vu_sel(hdrg_vm_t m, hdrg_vu_t a, hdrg_vu_t b)
{
    return wasm_v128_bitselect(a, b, m);
}

static inline hdrg_vm_t hdrg_vm_and(hdrg_vm_t a, hdrg_vm_t b)
{
    return wasm_v128_and(a, b);
}

static inline hdrg_vm_t hdrg_vm_or(hdrg_vm_t a, hdrg_vm_t b)
{
    return wasm_v128_or(a, b);
}

static inline hdrg_vm_t hdrg_vm_not(hdrg_vm_t a)
{
    return wasm_v128_not(a);
}

/* some lane of m is set */
static inline bool hdrg_vm_any(hdrg_vm_t m)
{
    return wasm_v128_any_true(m);
}

/* every lane of m is set */
static inline bool hdrg_vm_all(hdrg_vm_t m)
{
    return wasm_i32x4_all_true(m);
}

static inline hdrg_vu_t hdrg_vu_dup(uint32_t c)
{
    return wasm_u32x4_splat(c);
}

static inline hdrg_vu_t hdrg_vu_load_at(const uint32_t *p, uint32_t i)
{
    return wasm_v128_load(p + i);
}

/* the int32 lanes clamped to [0, 255] (saturating narrows), as bytes at p + i */
static inline void hdrg_vu_store_u8_at(uint8_t *p, uint32_t i, hdrg_vu_t v)
{
    v128_t half;
    int32_t bytes;

    half = wasm_i16x8_narrow_i32x4(v, v);
    bytes = wasm_i32x4_extract_lane(wasm_u8x16_narrow_i16x8(half, half), 0);
    memcpy(p + i, &bytes, sizeof(bytes));
}

/* the int32 lanes clamped to [0, 65535] (a saturating narrow), as 16-bit samples at p + i */
static inline void hdrg_vu_store_u16_at(uint16_t *p, uint32_t i, hdrg_vu_t v)
{
    wasm_v128_store64_lane(p + i, wasm_u16x8_narrow_i32x4(v, v), 0);
}

/* the lane indices 0 .. HDRG_V_LANES - 1 */
static inline hdrg_vu_t hdrg_vu_index(void)
{
    return wasm_u32x4_make(0, 1, 2, 3);
}

static inline hdrg_vu_t hdrg_vu_add(hdrg_vu_t a, hdrg_vu_t b)
{
    return wasm_i32x4_add(a, b);
}

static inline hdrg_vu_t hdrg_vu_sub(hdrg_vu_t a, hdrg_vu_t b)
{
    return wasm_i32x4_sub(a, b);
}

static inline hdrg_vu_t hdrg_vu_mul(hdrg_vu_t a, hdrg_vu_t b)
{
    return wasm_i32x4_mul(a, b);
}

static inline hdrg_vu_t hdrg_vu_min(hdrg_vu_t a, hdrg_vu_t b)
{
    return wasm_u32x4_min(a, b);
}

static inline hdrg_vu_t hdrg_vu_and(hdrg_vu_t a, hdrg_vu_t b)
{
    return wasm_v128_and(a, b);
}

static inline hdrg_vu_t hdrg_vu_or(hdrg_vu_t a, hdrg_vu_t b)
{
    return wasm_v128_or(a, b);
}

static inline hdrg_vu_t hdrg_vu_xor(hdrg_vu_t a, hdrg_vu_t b)
{
    return wasm_v128_xor(a, b);
}

/* the float and integer views share one type */
static inline hdrg_vu_t hdrg_vf_bits(hdrg_vf_t v)
{
    return v;
}

static inline hdrg_vf_t hdrg_vu_bits(hdrg_vu_t u)
{
    return u;
}

/* lanes as int32 -> float (exact below 2^24) */
static inline hdrg_vf_t hdrg_vf_from_i32(hdrg_vu_t u)
{
    return wasm_f32x4_convert_i32x4(u);
}

/* float -> int32 toward zero, as integer lanes */
static inline hdrg_vu_t hdrg_vu_trunc(hdrg_vf_t v)
{
    return wasm_i32x4_trunc_sat_f32x4(v);
}

/* float -> int32 to nearest even (lrintf in the default rounding mode) */
static inline hdrg_vu_t hdrg_vu_round(hdrg_vf_t v)
{
    return wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_nearest(v));
}

/* the lanes' (base[idx], base[idx + 1]) pairs as [a0 b0 a1 b1], [a2 b2 a3 b3] */
static inline void hdrg_wasm_pairs(const float *base, hdrg_vu_t idx, v128_t *lo, v128_t *hi)
{
    *lo = wasm_i32x4_shuffle(wasm_v128_load64_zero(base + wasm_u32x4_extract_lane(idx, 0)),
                             wasm_v128_load64_zero(base + wasm_u32x4_extract_lane(idx, 1)), 0, 1, 4,
                             5);
    *hi = wasm_i32x4_shuffle(wasm_v128_load64_zero(base + wasm_u32x4_extract_lane(idx, 2)),
                             wasm_v128_load64_zero(base + wasm_u32x4_extract_lane(idx, 3)), 0, 1, 4,
                             5);
}

/* base[idx], base[idx + 1], base[idx + 2] per lane: a pair and a single load each */
static inline void hdrg_vf_gather3(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b,
                                   hdrg_vf_t *c)
{
    v128_t lo, hi;

    hdrg_wasm_pairs(base, idx, &lo, &hi);

    *a = wasm_i32x4_shuffle(lo, hi, 0, 2, 4, 6);
    *b = wasm_i32x4_shuffle(lo, hi, 1, 3, 5, 7);
    *c = wasm_f32x4_make(
        base[wasm_u32x4_extract_lane(idx, 0) + 2u], base[wasm_u32x4_extract_lane(idx, 1) + 2u],
        base[wasm_u32x4_extract_lane(idx, 2) + 2u], base[wasm_u32x4_extract_lane(idx, 3) + 2u]);
}

/* base[idx], base[idx + 1] per lane: one pair load each */
static inline void hdrg_vf_gather2(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b)
{
    v128_t lo, hi;

    hdrg_wasm_pairs(base, idx, &lo, &hi);

    *a = wasm_i32x4_shuffle(lo, hi, 0, 2, 4, 6);
    *b = wasm_i32x4_shuffle(lo, hi, 1, 3, 5, 7);
}

#endif /* HDRG_KERNELS_V_WASM128_H */
