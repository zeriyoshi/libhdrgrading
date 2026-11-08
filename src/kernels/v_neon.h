/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_V_NEON_H
#define HDRG_KERNELS_V_NEON_H

#include <arm_neon.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * The vector primitives vmath.h is written in, for AArch64 Advanced SIMD (4
 * lanes). Multiplies and adds stay separate (vmulq / vaddq) except in
 * hdrg_vf_fma under HDRG_SIMD_FMA (vfmaq), so without it a sequence of them
 * rounds like the scalar reference.
 */
#define HDRG_V_LANES 4u
#ifdef HDRG_SIMD_FMA
#define HDRG_V_EXACT false
#else
#define HDRG_V_EXACT true
#endif

/* constant shifts: the intrinsics take immediates */
#define HDRG_VU_SHR(u, n) vshrq_n_u32((u), (n))
#define HDRG_VU_SHL(u, n) vshlq_n_u32((u), (n))

typedef float32x4_t hdrg_vf_t;
typedef uint32x4_t hdrg_vu_t;
typedef uint32x4_t hdrg_vm_t;

static inline hdrg_vf_t hdrg_vf_dup(float c)
{
    return vdupq_n_f32(c);
}

static inline hdrg_vf_t hdrg_vf_load_at(const float *p, uint32_t i)
{
    return vld1q_f32(p + i);
}

static inline void hdrg_vf_store_at(float *p, uint32_t i, hdrg_vf_t v)
{
    vst1q_f32(p + i, v);
}

static inline hdrg_vf_t hdrg_vf_add(hdrg_vf_t a, hdrg_vf_t b)
{
    return vaddq_f32(a, b);
}

static inline hdrg_vf_t hdrg_vf_sub(hdrg_vf_t a, hdrg_vf_t b)
{
    return vsubq_f32(a, b);
}

static inline hdrg_vf_t hdrg_vf_mul(hdrg_vf_t a, hdrg_vf_t b)
{
    return vmulq_f32(a, b);
}

static inline hdrg_vf_t hdrg_vf_div(hdrg_vf_t a, hdrg_vf_t b)
{
    return vdivq_f32(a, b);
}

#ifdef HDRG_SIMD_FMA
/* a * b + c, fused */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return vfmaq_f32(c, a, b);
}
#else
/* a * b + c with the scalar reference's two roundings */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return vaddq_f32(vmulq_f32(a, b), c);
}
#endif

static inline hdrg_vf_t hdrg_vf_neg(hdrg_vf_t a)
{
    return vnegq_f32(a);
}

static inline hdrg_vf_t hdrg_vf_abs(hdrg_vf_t a)
{
    return vabsq_f32(a);
}

static inline hdrg_vm_t hdrg_vf_lt(hdrg_vf_t a, hdrg_vf_t b)
{
    return vcltq_f32(a, b);
}

static inline hdrg_vm_t hdrg_vf_gt(hdrg_vf_t a, hdrg_vf_t b)
{
    return vcgtq_f32(a, b);
}

static inline hdrg_vm_t hdrg_vf_le(hdrg_vf_t a, hdrg_vf_t b)
{
    return vcleq_f32(a, b);
}

static inline hdrg_vm_t hdrg_vf_ge(hdrg_vf_t a, hdrg_vf_t b)
{
    return vcgeq_f32(a, b);
}

/* m ? a : b per lane */
static inline hdrg_vf_t hdrg_vf_sel(hdrg_vm_t m, hdrg_vf_t a, hdrg_vf_t b)
{
    return vbslq_f32(m, a, b);
}

static inline hdrg_vu_t hdrg_vu_sel(hdrg_vm_t m, hdrg_vu_t a, hdrg_vu_t b)
{
    return vbslq_u32(m, a, b);
}

static inline hdrg_vm_t hdrg_vm_and(hdrg_vm_t a, hdrg_vm_t b)
{
    return vandq_u32(a, b);
}

static inline hdrg_vm_t hdrg_vm_or(hdrg_vm_t a, hdrg_vm_t b)
{
    return vorrq_u32(a, b);
}

static inline hdrg_vm_t hdrg_vm_not(hdrg_vm_t a)
{
    return vmvnq_u32(a);
}

/* some lane of m is set */
static inline bool hdrg_vm_any(hdrg_vm_t m)
{
    return vmaxvq_u32(m) != 0;
}

/* every lane of m is set */
static inline bool hdrg_vm_all(hdrg_vm_t m)
{
    return vminvq_u32(m) != 0;
}

static inline hdrg_vu_t hdrg_vu_dup(uint32_t c)
{
    return vdupq_n_u32(c);
}

static inline hdrg_vu_t hdrg_vu_load_at(const uint32_t *p, uint32_t i)
{
    return vld1q_u32(p + i);
}

/* the int32 lanes clamped to [0, 255] (saturating narrows), as bytes at p + i */
static inline void hdrg_vu_store_u8_at(uint8_t *p, uint32_t i, hdrg_vu_t v)
{
    uint16x4_t half;
    uint32_t bytes;

    half = vqmovun_s32(vreinterpretq_s32_u32(v));
    bytes = vget_lane_u32(vreinterpret_u32_u8(vqmovn_u16(vcombine_u16(half, half))), 0);
    memcpy(p + i, &bytes, sizeof(bytes));
}

/* the int32 lanes clamped to [0, 65535] (a saturating narrow), as 16-bit samples at p + i */
static inline void hdrg_vu_store_u16_at(uint16_t *p, uint32_t i, hdrg_vu_t v)
{
    vst1_u16(p + i, vqmovun_s32(vreinterpretq_s32_u32(v)));
}

/* the lane indices 0 .. HDRG_V_LANES - 1 */
static inline hdrg_vu_t hdrg_vu_index(void)
{
    static const uint32_t index[HDRG_V_LANES] = {0, 1, 2, 3};

    return vld1q_u32(index);
}

static inline hdrg_vu_t hdrg_vu_add(hdrg_vu_t a, hdrg_vu_t b)
{
    return vaddq_u32(a, b);
}

static inline hdrg_vu_t hdrg_vu_sub(hdrg_vu_t a, hdrg_vu_t b)
{
    return vsubq_u32(a, b);
}

static inline hdrg_vu_t hdrg_vu_mul(hdrg_vu_t a, hdrg_vu_t b)
{
    return vmulq_u32(a, b);
}

static inline hdrg_vu_t hdrg_vu_min(hdrg_vu_t a, hdrg_vu_t b)
{
    return vminq_u32(a, b);
}

static inline hdrg_vu_t hdrg_vu_and(hdrg_vu_t a, hdrg_vu_t b)
{
    return vandq_u32(a, b);
}

static inline hdrg_vu_t hdrg_vu_or(hdrg_vu_t a, hdrg_vu_t b)
{
    return vorrq_u32(a, b);
}

static inline hdrg_vu_t hdrg_vu_xor(hdrg_vu_t a, hdrg_vu_t b)
{
    return veorq_u32(a, b);
}

static inline hdrg_vu_t hdrg_vf_bits(hdrg_vf_t v)
{
    return vreinterpretq_u32_f32(v);
}

static inline hdrg_vf_t hdrg_vu_bits(hdrg_vu_t u)
{
    return vreinterpretq_f32_u32(u);
}

/* lanes as int32 -> float (exact below 2^24) */
static inline hdrg_vf_t hdrg_vf_from_i32(hdrg_vu_t u)
{
    return vcvtq_f32_s32(vreinterpretq_s32_u32(u));
}

/* float -> int32 toward zero, as integer lanes */
static inline hdrg_vu_t hdrg_vu_trunc(hdrg_vf_t v)
{
    return vreinterpretq_u32_s32(vcvtq_s32_f32(v));
}

/* float -> int32 to nearest even (lrintf under the pinned rounding mode) */
static inline hdrg_vu_t hdrg_vu_round(hdrg_vf_t v)
{
    return vreinterpretq_u32_s32(vcvtnq_s32_f32(v));
}

/* base[idx], base[idx + 1], base[idx + 2] per lane: one structure load each */
static inline void hdrg_vf_gather3(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b,
                                   hdrg_vf_t *c)
{
    float32x4x3_t v;

    v = vld3q_dup_f32(base + vgetq_lane_u32(idx, 0));
    v = vld3q_lane_f32(base + vgetq_lane_u32(idx, 1), v, 1);
    v = vld3q_lane_f32(base + vgetq_lane_u32(idx, 2), v, 2);
    v = vld3q_lane_f32(base + vgetq_lane_u32(idx, 3), v, 3);

    *a = v.val[0];
    *b = v.val[1];
    *c = v.val[2];
}

/* base[idx], base[idx + 1] per lane: one structure load each */
static inline void hdrg_vf_gather2(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b)
{
    float32x4x2_t v;

    v = vld2q_dup_f32(base + vgetq_lane_u32(idx, 0));
    v = vld2q_lane_f32(base + vgetq_lane_u32(idx, 1), v, 1);
    v = vld2q_lane_f32(base + vgetq_lane_u32(idx, 2), v, 2);
    v = vld2q_lane_f32(base + vgetq_lane_u32(idx, 3), v, 3);

    *a = v.val[0];
    *b = v.val[1];
}

#endif /* HDRG_KERNELS_V_NEON_H */
