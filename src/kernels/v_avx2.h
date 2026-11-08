/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_V_AVX2_H
#define HDRG_KERNELS_V_AVX2_H

#include <immintrin.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * The vector primitives vmath.h is written in, for AVX2 + FMA (8 lanes; the
 * translation unit is built with -mavx2 -mfma). Multiplies and adds stay
 * separate except in hdrg_vf_fma under HDRG_SIMD_FMA, so without it they
 * round like the scalar reference. cvtps rounds with the pinned MXCSR.
 * Ordered quiet predicates give the scalar comparisons' results.
 */
#define HDRG_V_LANES 8u
#ifdef HDRG_SIMD_FMA
#define HDRG_V_EXACT false
#else
#define HDRG_V_EXACT true
#endif

/* constant shifts: the intrinsics take immediates */
#define HDRG_VU_SHR(u, n) _mm256_srli_epi32((u), (n))
#define HDRG_VU_SHL(u, n) _mm256_slli_epi32((u), (n))

typedef __m256 hdrg_vf_t;
typedef __m256i hdrg_vu_t;
typedef __m256 hdrg_vm_t;

static inline hdrg_vf_t hdrg_vf_dup(float c)
{
    return _mm256_set1_ps(c);
}

static inline hdrg_vf_t hdrg_vf_load_at(const float *p, uint32_t i)
{
    return _mm256_loadu_ps(p + i);
}

static inline void hdrg_vf_store_at(float *p, uint32_t i, hdrg_vf_t v)
{
    _mm256_storeu_ps(p + i, v);
}

static inline hdrg_vf_t hdrg_vf_add(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_add_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_sub(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_sub_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_mul(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_mul_ps(a, b);
}

static inline hdrg_vf_t hdrg_vf_div(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_div_ps(a, b);
}

#ifdef HDRG_SIMD_FMA
/* a * b + c, fused */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return _mm256_fmadd_ps(a, b, c);
}
#else
/* a * b + c with the scalar reference's two roundings */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return _mm256_add_ps(_mm256_mul_ps(a, b), c);
}
#endif

static inline hdrg_vf_t hdrg_vf_neg(hdrg_vf_t a)
{
    return _mm256_xor_ps(a, _mm256_set1_ps(-0.0f));
}

static inline hdrg_vf_t hdrg_vf_abs(hdrg_vf_t a)
{
    return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a);
}

static inline hdrg_vm_t hdrg_vf_lt(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_cmp_ps(a, b, _CMP_LT_OQ);
}

static inline hdrg_vm_t hdrg_vf_gt(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_cmp_ps(a, b, _CMP_GT_OQ);
}

static inline hdrg_vm_t hdrg_vf_le(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_cmp_ps(a, b, _CMP_LE_OQ);
}

static inline hdrg_vm_t hdrg_vf_ge(hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_cmp_ps(a, b, _CMP_GE_OQ);
}

/* m ? a : b per lane */
static inline hdrg_vf_t hdrg_vf_sel(hdrg_vm_t m, hdrg_vf_t a, hdrg_vf_t b)
{
    return _mm256_blendv_ps(b, a, m);
}

static inline hdrg_vu_t hdrg_vu_sel(hdrg_vm_t m, hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(b), _mm256_castsi256_ps(a), m));
}

static inline hdrg_vm_t hdrg_vm_and(hdrg_vm_t a, hdrg_vm_t b)
{
    return _mm256_and_ps(a, b);
}

static inline hdrg_vm_t hdrg_vm_or(hdrg_vm_t a, hdrg_vm_t b)
{
    return _mm256_or_ps(a, b);
}

static inline hdrg_vm_t hdrg_vm_not(hdrg_vm_t a)
{
    return _mm256_xor_ps(a, _mm256_castsi256_ps(_mm256_set1_epi32(-1)));
}

/* some lane of m is set */
static inline bool hdrg_vm_any(hdrg_vm_t m)
{
    return _mm256_movemask_ps(m) != 0;
}

/* every lane of m is set */
static inline bool hdrg_vm_all(hdrg_vm_t m)
{
    return _mm256_movemask_ps(m) == 0xFF;
}

static inline hdrg_vu_t hdrg_vu_dup(uint32_t c)
{
    return _mm256_set1_epi32((int32_t)c);
}

static inline hdrg_vu_t hdrg_vu_load_at(const uint32_t *p, uint32_t i)
{
    return _mm256_loadu_si256((const __m256i *)(p + i));
}

/* the int32 lanes clamped to [0, 255] (saturating packs), as bytes at p + i */
static inline void hdrg_vu_store_u8_at(uint8_t *p, uint32_t i, hdrg_vu_t v)
{
    __m128i half;
    int64_t bytes;

    half = _mm_packs_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    bytes = _mm_cvtsi128_si64(_mm_packus_epi16(half, half));
    memcpy(p + i, &bytes, sizeof(bytes));
}

/* the int32 lanes clamped to [0, 65535] (a saturating pack), as 16-bit samples at p + i */
static inline void hdrg_vu_store_u16_at(uint16_t *p, uint32_t i, hdrg_vu_t v)
{
    _mm_storeu_si128((__m128i *)(p + i),
                     _mm_packus_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)));
}

/* the lane indices 0 .. HDRG_V_LANES - 1 */
static inline hdrg_vu_t hdrg_vu_index(void)
{
    return _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
}

static inline hdrg_vu_t hdrg_vu_add(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm256_add_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_sub(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm256_sub_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_mul(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm256_mullo_epi32(a, b);
}

static inline hdrg_vu_t hdrg_vu_min(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm256_min_epu32(a, b);
}

static inline hdrg_vu_t hdrg_vu_and(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm256_and_si256(a, b);
}

static inline hdrg_vu_t hdrg_vu_or(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm256_or_si256(a, b);
}

static inline hdrg_vu_t hdrg_vu_xor(hdrg_vu_t a, hdrg_vu_t b)
{
    return _mm256_xor_si256(a, b);
}

static inline hdrg_vu_t hdrg_vf_bits(hdrg_vf_t v)
{
    return _mm256_castps_si256(v);
}

static inline hdrg_vf_t hdrg_vu_bits(hdrg_vu_t u)
{
    return _mm256_castsi256_ps(u);
}

/* lanes as int32 -> float (exact below 2^24) */
static inline hdrg_vf_t hdrg_vf_from_i32(hdrg_vu_t u)
{
    return _mm256_cvtepi32_ps(u);
}

/* float -> int32 toward zero, as integer lanes */
static inline hdrg_vu_t hdrg_vu_trunc(hdrg_vf_t v)
{
    return _mm256_cvttps_epi32(v);
}

/* float -> int32 to nearest even (lrintf under the pinned rounding mode) */
static inline hdrg_vu_t hdrg_vu_round(hdrg_vf_t v)
{
    return _mm256_cvtps_epi32(v);
}

/* base[idx], base[idx + 1], base[idx + 2] per lane (indices below 2^31) */
static inline void hdrg_vf_gather3(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b,
                                   hdrg_vf_t *c)
{
    *a = _mm256_i32gather_ps(base, idx, 4);
    *b = _mm256_i32gather_ps(base + 1, idx, 4);
    *c = _mm256_i32gather_ps(base + 2, idx, 4);
}

/* base[idx], base[idx + 1] per lane (indices below 2^31) */
static inline void hdrg_vf_gather2(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b)
{
    *a = _mm256_i32gather_ps(base, idx, 4);
    *b = _mm256_i32gather_ps(base + 1, idx, 4);
}

#endif /* HDRG_KERNELS_V_AVX2_H */
