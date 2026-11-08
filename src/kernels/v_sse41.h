/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_V_SSE41_H
#define HDRG_KERNELS_V_SSE41_H

#include <smmintrin.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * The vector primitives vmath.h is written in, for SSE4.1 (4 lanes; the
 * translation unit is built with -msse4.1). SSE has no fused multiply-add:
 * the tier is exact under HDRG_SIMD_FMA too. cvtps rounds with the pinned
 * MXCSR: to nearest even.
 */
#define HDRG_V_LANES 4u
#define HDRG_V_EXACT true

/* constant shifts: the intrinsics take immediates */
#define HDRG_VU_SHR(u, n) _mm_srli_epi32((u), (n))
#define HDRG_VU_SHL(u, n) _mm_slli_epi32((u), (n))

typedef __m128 hdrg_vf_t;
typedef __m128i hdrg_vu_t;
typedef __m128 hdrg_vm_t;

static inline hdrg_vf_t hdrg_vf_dup(float c)
{
    return _mm_set1_ps(c);
}

static inline hdrg_vf_t hdrg_vf_load_at(const float *p, uint32_t i)
{
    return _mm_loadu_ps(p + i);
}

static inline void hdrg_vf_store_at(float *p, uint32_t i, hdrg_vf_t v)
{
    _mm_storeu_ps(p + i, v);
}

static inline hdrg_vf_t hdrg_vf_add(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_add_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_sub(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_sub_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_mul(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_mul_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_div(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_div_ps(a, b);
}

/* a * b + c with the scalar reference's two roundings */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return _mm_add_ps(_mm_mul_ps(a, b), c);
}

static inline hdrg_vf_t hdrg_vf_neg(hdrg_vf_t a)
{
    return _mm_xor_ps(a, _mm_set1_ps(-0.0f));
}

static inline hdrg_vf_t hdrg_vf_abs(hdrg_vf_t a)
{
    return _mm_andnot_ps(_mm_set1_ps(-0.0f), a);
}

static inline hdrg_vm_t hdrg_vf_lt(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_cmplt_ps(a, b);
}

static inline hdrg_vm_t hdrg_vf_gt(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_cmpgt_ps(a, b);
}

static inline hdrg_vm_t hdrg_vf_le(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_cmple_ps(a, b);
}

static inline hdrg_vm_t hdrg_vf_ge(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_cmpge_ps(a, b);
}

/* m ? a : b per lane */
static inline hdrg_vf_t hdrg_vf_sel(hdrg_vm_t m, hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm_blendv_ps(b, a, m);
}

static inline hdrg_vu_t hdrg_vu_sel(hdrg_vm_t m, hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(b), _mm_castsi128_ps(a), m));
}

static inline hdrg_vm_t hdrg_vm_and(hdrg_vm_t a, hdrg_vm_t b)
{
    return _mm_and_ps(a, b);
}

static inline hdrg_vm_t hdrg_vm_or(hdrg_vm_t a, hdrg_vm_t b)
{
    return _mm_or_ps(a, b);
}

static inline hdrg_vm_t hdrg_vm_not(hdrg_vm_t a)
{
    return _mm_xor_ps(a, _mm_castsi128_ps(_mm_set1_epi32(-1)));
}

/* some lane of m is set */
static inline bool hdrg_vm_any(hdrg_vm_t m)
{
    return _mm_movemask_ps(m) != 0;
}

/* every lane of m is set */
static inline bool hdrg_vm_all(hdrg_vm_t m)
{
    return _mm_movemask_ps(m) == 0xF;
}

static inline hdrg_vu_t hdrg_vu_dup(uint32_t c)
{
    return _mm_set1_epi32((int32_t)c);
}

static inline hdrg_vu_t hdrg_vu_load_at(const uint32_t *p, uint32_t i)
{
    return _mm_loadu_si128((const __m128i *)(p + i));
}

/* the int32 lanes clamped to [0, 255] (saturating packs), as bytes at p + i */
static inline void hdrg_vu_store_u8_at(uint8_t *p, uint32_t i, hdrg_vu_t v)
{
    __m128i half;
    int32_t bytes;

    half = _mm_packs_epi32(v, v);
    bytes = _mm_cvtsi128_si32(_mm_packus_epi16(half, half));
    memcpy(p + i, &bytes, sizeof(bytes));
}

/* the int32 lanes clamped to [0, 65535] (a saturating pack), as 16-bit samples at p + i */
static inline void hdrg_vu_store_u16_at(uint16_t *p, uint32_t i, hdrg_vu_t v)
{
    _mm_storel_epi64((__m128i *)(p + i), _mm_packus_epi32(v, v));
}

/* the lane indices 0 .. HDRG_V_LANES - 1 */
static inline hdrg_vu_t hdrg_vu_index(void)
{
    return _mm_setr_epi32(0, 1, 2, 3);
}

static inline hdrg_vu_t hdrg_vu_add(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm_add_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_sub(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm_sub_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_mul(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm_mullo_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_min(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm_min_epu32(a, b);
}

static inline hdrg_vu_t hdrg_vu_and(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm_and_si128(a, b);
}

static inline hdrg_vu_t hdrg_vu_or(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm_or_si128(a, b);
}

static inline hdrg_vu_t hdrg_vu_xor(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm_xor_si128(a, b);
}

static inline hdrg_vu_t hdrg_vf_bits(hdrg_vf_t v)
{
    return _mm_castps_si128(v);
}

static inline hdrg_vf_t hdrg_vu_bits(hdrg_vu_t u)
{
    return _mm_castsi128_ps(u);
}

/* lanes as int32 -> float (exact below 2^24) */
static inline hdrg_vf_t hdrg_vf_from_i32(hdrg_vu_t u)
{
    return _mm_cvtepi32_ps(u);
}

/* float -> int32 toward zero, as integer lanes */
static inline hdrg_vu_t hdrg_vu_trunc(hdrg_vf_t v)
{
    return _mm_cvttps_epi32(v);
}

/* float -> int32 to nearest even (lrintf under the pinned rounding mode) */
static inline hdrg_vu_t hdrg_vu_round(hdrg_vf_t v)
{
    return _mm_cvtps_epi32(v);
}

/* the lanes' (base[idx], base[idx + 1]) pairs as [a0 b0 a1 b1], [a2 b2 a3 b3] */
static inline void hdrg_sse_pairs(const float *base, hdrg_vu_t idx, __m128 *lo, __m128 *hi)
{
    const __m128 zero = _mm_setzero_ps();

    *lo = _mm_movelh_ps(
        _mm_loadl_pi(zero, (const __m64 *)(base + (uint32_t)_mm_cvtsi128_si32(idx))),
        _mm_loadl_pi(zero, (const __m64 *)(base + (uint32_t)_mm_extract_epi32(idx, 1))));
    *hi = _mm_movelh_ps(
        _mm_loadl_pi(zero, (const __m64 *)(base + (uint32_t)_mm_extract_epi32(idx, 2))),
        _mm_loadl_pi(zero, (const __m64 *)(base + (uint32_t)_mm_extract_epi32(idx, 3))));
}

/* base[idx], base[idx + 1], base[idx + 2] per lane: a pair and a single load each */
static inline void hdrg_vf_gather3(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b,
                                   hdrg_vf_t *c)
{
    __m128 lo, hi;

    hdrg_sse_pairs(base, idx, &lo, &hi);

    *a = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));
    *b = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));
    *c = _mm_setr_ps(base[(uint32_t)_mm_cvtsi128_si32(idx) + 2u],
                     base[(uint32_t)_mm_extract_epi32(idx, 1) + 2u],
                     base[(uint32_t)_mm_extract_epi32(idx, 2) + 2u],
                     base[(uint32_t)_mm_extract_epi32(idx, 3) + 2u]);
}

/* base[idx], base[idx + 1] per lane: one pair load each */
static inline void hdrg_vf_gather2(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b)
{
    __m128 lo, hi;

    hdrg_sse_pairs(base, idx, &lo, &hi);

    *a = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));
    *b = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));
}

#endif /* HDRG_KERNELS_V_SSE41_H */
