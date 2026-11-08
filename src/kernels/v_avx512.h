/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_V_AVX512_H
#define HDRG_KERNELS_V_AVX512_H

#include <immintrin.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * The vector primitives vmath.h is written in, for AVX-512 F / DQ / BW / VL
 * (16 lanes: a block is one group; provisional). Masks
 * are k registers. Multiplies and adds stay separate except in hdrg_vf_fma
 * under HDRG_SIMD_FMA. cvtps rounds with the pinned MXCSR.
 */
#define HDRG_V_LANES 16u
#ifdef HDRG_SIMD_FMA
#define HDRG_V_EXACT false
#else
#define HDRG_V_EXACT true
#endif

/* constant shifts: the intrinsics take immediates */
#define HDRG_VU_SHR(u, n) _mm512_srli_epi32((u), (n))
#define HDRG_VU_SHL(u, n) _mm512_slli_epi32((u), (n))

typedef __m512 hdrg_vf_t;
typedef __m512i hdrg_vu_t;
typedef __mmask16 hdrg_vm_t;

static inline hdrg_vf_t hdrg_vf_dup(float c)
{
    return _mm512_set1_ps(c);
}

static inline hdrg_vf_t hdrg_vf_load_at(const float *p, uint32_t i)
{
    return _mm512_loadu_ps(p + i);
}

static inline void hdrg_vf_store_at(float *p, uint32_t i, hdrg_vf_t v)
{
    _mm512_storeu_ps(p + i, v);
}

static inline hdrg_vf_t hdrg_vf_add(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_add_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_sub(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_sub_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_mul(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_mul_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_div(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_div_ps(a, b);
}

#ifdef HDRG_SIMD_FMA
/* a * b + c, fused */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return _mm512_fmadd_ps(a, b, c);
}
#else
/* a * b + c with the scalar reference's two roundings */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return _mm512_add_ps(_mm512_mul_ps(a, b), c);
}
#endif

static inline hdrg_vf_t hdrg_vf_neg(hdrg_vf_t a)
{
    return _mm512_xor_ps(a, _mm512_set1_ps(-0.0f));
}

static inline hdrg_vf_t hdrg_vf_abs(hdrg_vf_t a)
{
    return _mm512_andnot_ps(_mm512_set1_ps(-0.0f), a);
}

static inline hdrg_vm_t hdrg_vf_lt(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_cmp_ps_mask(a, b, _CMP_LT_OQ);
}

static inline hdrg_vm_t hdrg_vf_gt(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_cmp_ps_mask(a, b, _CMP_GT_OQ);
}

static inline hdrg_vm_t hdrg_vf_le(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_cmp_ps_mask(a, b, _CMP_LE_OQ);
}

static inline hdrg_vm_t hdrg_vf_ge(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_cmp_ps_mask(a, b, _CMP_GE_OQ);
}

/* m ? a : b per lane */
static inline hdrg_vf_t hdrg_vf_sel(hdrg_vm_t m, hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm512_mask_blend_ps(m, b, a);
}

static inline hdrg_vu_t hdrg_vu_sel(hdrg_vm_t m, hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm512_mask_blend_epi32(m, b, a);
}

static inline hdrg_vm_t hdrg_vm_and(hdrg_vm_t a, hdrg_vm_t b)
{
    return _kand_mask16(a, b);
}

static inline hdrg_vm_t hdrg_vm_or(hdrg_vm_t a, hdrg_vm_t b)
{
    return _kor_mask16(a, b);
}

static inline hdrg_vm_t hdrg_vm_not(hdrg_vm_t a)
{
    return _knot_mask16(a);
}

/* some lane of m is set */
static inline bool hdrg_vm_any(hdrg_vm_t m)
{
    return m != 0;
}

/* every lane of m is set */
static inline bool hdrg_vm_all(hdrg_vm_t m)
{
    return m == 0xFFFF;
}

static inline hdrg_vu_t hdrg_vu_dup(uint32_t c)
{
    return _mm512_set1_epi32((int32_t)c);
}

static inline hdrg_vu_t hdrg_vu_load_at(const uint32_t *p, uint32_t i)
{
    return _mm512_loadu_si512(p + i);
}

/* the int32 lanes clamped to [0, 255], as bytes at p + i */
static inline void hdrg_vu_store_u8_at(uint8_t *p, uint32_t i, hdrg_vu_t v)
{
    _mm_storeu_si128((__m128i *)(p + i),
                     _mm512_cvtusepi32_epi8(_mm512_max_epi32(v, _mm512_setzero_si512())));
}

/* the int32 lanes clamped to [0, 65535], as 16-bit samples at p + i */
static inline void hdrg_vu_store_u16_at(uint16_t *p, uint32_t i, hdrg_vu_t v)
{
    _mm256_storeu_si256((__m256i *)(p + i),
                        _mm512_cvtusepi32_epi16(_mm512_max_epi32(v, _mm512_setzero_si512())));
}

/* the lane indices 0 .. HDRG_V_LANES - 1 */
static inline hdrg_vu_t hdrg_vu_index(void)
{
    return _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
}

static inline hdrg_vu_t hdrg_vu_add(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm512_add_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_sub(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm512_sub_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_mul(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm512_mullo_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_min(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm512_min_epu32(a, b);
}

static inline hdrg_vu_t hdrg_vu_and(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm512_and_si512(a, b);
}

static inline hdrg_vu_t hdrg_vu_or(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm512_or_si512(a, b);
}

static inline hdrg_vu_t hdrg_vu_xor(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm512_xor_si512(a, b);
}

static inline hdrg_vu_t hdrg_vf_bits(hdrg_vf_t v)
{
    return _mm512_castps_si512(v);
}

static inline hdrg_vf_t hdrg_vu_bits(hdrg_vu_t u)
{
    return _mm512_castsi512_ps(u);
}

/* lanes as int32 -> float (exact below 2^24) */
static inline hdrg_vf_t hdrg_vf_from_i32(hdrg_vu_t u)
{
    return _mm512_cvtepi32_ps(u);
}

/* float -> int32 toward zero, as integer lanes */
static inline hdrg_vu_t hdrg_vu_trunc(hdrg_vf_t v)
{
    return _mm512_cvttps_epi32(v);
}

/* float -> int32 to nearest even (lrintf under the pinned rounding mode) */
static inline hdrg_vu_t hdrg_vu_round(hdrg_vf_t v)
{
    return _mm512_cvtps_epi32(v);
}

/* base[idx], base[idx + 1], base[idx + 2] per lane (indices below 2^31) */
static inline void hdrg_vf_gather3(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b,
                                   hdrg_vf_t *c)
{
    *a = _mm512_i32gather_ps(idx, base, 4);
    *b = _mm512_i32gather_ps(idx, base + 1, 4);
    *c = _mm512_i32gather_ps(idx, base + 2, 4);
}

/* base[idx], base[idx + 1] per lane (indices below 2^31) */
static inline void hdrg_vf_gather2(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b)
{
    *a = _mm512_i32gather_ps(idx, base, 4);
    *b = _mm512_i32gather_ps(idx, base + 1, 4);
}

#endif /* HDRG_KERNELS_V_AVX512_H */
