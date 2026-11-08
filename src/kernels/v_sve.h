/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_V_SVE_H
#define HDRG_KERNELS_V_SVE_H

#include <arm_sve.h>
#include <stdbool.h>
#include <stdint.h>

#include "kernels.h"

/*
 * The vector primitives vmath.h is written in, for SVE (built with
 * -march=armv8.2-a+sve; provisional). Vector-length agnostic:
 * a group is svcntw() lanes, loads and stores at lane i of a block are
 * predicated to the block, and the arithmetic runs on every lane, so a
 * lane's value never depends on the vector length. svrintn rounds to
 * nearest even like lrintf under the pinned FPCR.
 */
#define HDRG_V_LANES ((uint32_t)svcntw())
#ifdef HDRG_SIMD_FMA
#define HDRG_V_EXACT false
#else
#define HDRG_V_EXACT true
#endif

/* constant shifts */
#define HDRG_VU_SHR(u, n) svlsr_n_u32_x(svptrue_b32(), (u), (n))
#define HDRG_VU_SHL(u, n) svlsl_n_u32_x(svptrue_b32(), (u), (n))

typedef svfloat32_t hdrg_vf_t;
typedef svuint32_t hdrg_vu_t;
typedef svbool_t hdrg_vm_t;

/* the lanes of a group at lane i that lie inside the block */
static inline svbool_t hdrg_v_active(uint32_t i)
{
    return svwhilelt_b32_u32(i, HDRG_KERNEL_BLOCK);
}

static inline hdrg_vf_t hdrg_vf_dup(float c)
{
    return svdup_n_f32(c);
}

static inline hdrg_vf_t hdrg_vf_load_at(const float *p, uint32_t i)
{
    return svld1_f32(hdrg_v_active(i), p + i);
}

static inline void hdrg_vf_store_at(float *p, uint32_t i, hdrg_vf_t v)
{
    svst1_f32(hdrg_v_active(i), p + i, v);
}

static inline hdrg_vf_t hdrg_vf_add(hdrg_vf_t a, hdrg_vf_t b)
{
    return svadd_f32_x(svptrue_b32(), a, b);
}

static inline hdrg_vf_t hdrg_vf_sub(hdrg_vf_t a, hdrg_vf_t b)
{
    return svsub_f32_x(svptrue_b32(), a, b);
}

static inline hdrg_vf_t hdrg_vf_mul(hdrg_vf_t a, hdrg_vf_t b)
{
    return svmul_f32_x(svptrue_b32(), a, b);
}

static inline hdrg_vf_t hdrg_vf_div(hdrg_vf_t a, hdrg_vf_t b)
{
    return svdiv_f32_x(svptrue_b32(), a, b);
}

#ifdef HDRG_SIMD_FMA
/* a * b + c, fused */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return svmla_f32_x(svptrue_b32(), c, a, b);
}
#else
/* a * b + c with the scalar reference's two roundings */
static inline hdrg_vf_t hdrg_vf_fma(hdrg_vf_t a, hdrg_vf_t b, hdrg_vf_t c)
{
    return svadd_f32_x(svptrue_b32(), svmul_f32_x(svptrue_b32(), a, b), c);
}
#endif

static inline hdrg_vf_t hdrg_vf_neg(hdrg_vf_t a)
{
    return svneg_f32_x(svptrue_b32(), a);
}

static inline hdrg_vf_t hdrg_vf_abs(hdrg_vf_t a)
{
    return svabs_f32_x(svptrue_b32(), a);
}

static inline hdrg_vm_t hdrg_vf_lt(hdrg_vf_t a, hdrg_vf_t b)
{
    return svcmplt_f32(svptrue_b32(), a, b);
}

static inline hdrg_vm_t hdrg_vf_gt(hdrg_vf_t a, hdrg_vf_t b)
{
    return svcmpgt_f32(svptrue_b32(), a, b);
}

static inline hdrg_vm_t hdrg_vf_le(hdrg_vf_t a, hdrg_vf_t b)
{
    return svcmple_f32(svptrue_b32(), a, b);
}

static inline hdrg_vm_t hdrg_vf_ge(hdrg_vf_t a, hdrg_vf_t b)
{
    return svcmpge_f32(svptrue_b32(), a, b);
}

/* m ? a : b per lane */
static inline hdrg_vf_t hdrg_vf_sel(hdrg_vm_t m, hdrg_vf_t a, hdrg_vf_t b)
{
    return svsel_f32(m, a, b);
}

static inline hdrg_vu_t hdrg_vu_sel(hdrg_vm_t m, hdrg_vu_t a, hdrg_vu_t b)
{
    return svsel_u32(m, a, b);
}

static inline hdrg_vm_t hdrg_vm_and(hdrg_vm_t a, hdrg_vm_t b)
{
    return svand_b_z(svptrue_b32(), a, b);
}

static inline hdrg_vm_t hdrg_vm_or(hdrg_vm_t a, hdrg_vm_t b)
{
    return svorr_b_z(svptrue_b32(), a, b);
}

static inline hdrg_vm_t hdrg_vm_not(hdrg_vm_t a)
{
    return svnot_b_z(svptrue_b32(), a);
}

/* some lane of m is set */
static inline bool hdrg_vm_any(hdrg_vm_t m)
{
    return svptest_any(svptrue_b32(), m);
}

/* every lane of m is set */
static inline bool hdrg_vm_all(hdrg_vm_t m)
{
    return !svptest_any(svptrue_b32(), svnot_b_z(svptrue_b32(), m));
}

static inline hdrg_vu_t hdrg_vu_dup(uint32_t c)
{
    return svdup_n_u32(c);
}

static inline hdrg_vu_t hdrg_vu_load_at(const uint32_t *p, uint32_t i)
{
    return svld1_u32(hdrg_v_active(i), p + i);
}

/* the int32 lanes clamped to [0, 255], as bytes at p + i */
static inline void hdrg_vu_store_u8_at(uint8_t *p, uint32_t i, hdrg_vu_t v)
{
    const svbool_t all = svptrue_b32();

    svst1b_u32(hdrg_v_active(i), p + i,
               svreinterpret_u32_s32(
                   svmin_n_s32_x(all, svmax_n_s32_x(all, svreinterpret_s32_u32(v), 0), 255)));
}

/* the int32 lanes clamped to [0, 65535], as 16-bit samples at p + i */
static inline void hdrg_vu_store_u16_at(uint16_t *p, uint32_t i, hdrg_vu_t v)
{
    const svbool_t all = svptrue_b32();

    svst1h_u32(hdrg_v_active(i), p + i,
               svreinterpret_u32_s32(
                   svmin_n_s32_x(all, svmax_n_s32_x(all, svreinterpret_s32_u32(v), 0), 65535)));
}

/* the lane indices 0 .. HDRG_V_LANES - 1 */
static inline hdrg_vu_t hdrg_vu_index(void)
{
    return svindex_u32(0, 1);
}

static inline hdrg_vu_t hdrg_vu_add(hdrg_vu_t a, hdrg_vu_t b)
{
    return svadd_u32_x(svptrue_b32(), a, b);
}

static inline hdrg_vu_t hdrg_vu_sub(hdrg_vu_t a, hdrg_vu_t b)
{
    return svsub_u32_x(svptrue_b32(), a, b);
}

static inline hdrg_vu_t hdrg_vu_mul(hdrg_vu_t a, hdrg_vu_t b)
{
    return svmul_u32_x(svptrue_b32(), a, b);
}

static inline hdrg_vu_t hdrg_vu_min(hdrg_vu_t a, hdrg_vu_t b)
{
    return svmin_u32_x(svptrue_b32(), a, b);
}

static inline hdrg_vu_t hdrg_vu_and(hdrg_vu_t a, hdrg_vu_t b)
{
    return svand_u32_x(svptrue_b32(), a, b);
}

static inline hdrg_vu_t hdrg_vu_or(hdrg_vu_t a, hdrg_vu_t b)
{
    return svorr_u32_x(svptrue_b32(), a, b);
}

static inline hdrg_vu_t hdrg_vu_xor(hdrg_vu_t a, hdrg_vu_t b)
{
    return sveor_u32_x(svptrue_b32(), a, b);
}

static inline hdrg_vu_t hdrg_vf_bits(hdrg_vf_t v)
{
    return svreinterpret_u32_f32(v);
}

static inline hdrg_vf_t hdrg_vu_bits(hdrg_vu_t u)
{
    return svreinterpret_f32_u32(u);
}

/* lanes as int32 -> float (exact below 2^24) */
static inline hdrg_vf_t hdrg_vf_from_i32(hdrg_vu_t u)
{
    return svcvt_f32_s32_x(svptrue_b32(), svreinterpret_s32_u32(u));
}

/* float -> int32 toward zero, as integer lanes */
static inline hdrg_vu_t hdrg_vu_trunc(hdrg_vf_t v)
{
    return svreinterpret_u32_s32(svcvt_s32_f32_x(svptrue_b32(), v));
}

/* float -> int32 to nearest even (lrintf under the pinned rounding mode) */
static inline hdrg_vu_t hdrg_vu_round(hdrg_vf_t v)
{
    return svreinterpret_u32_s32(svcvt_s32_f32_x(svptrue_b32(), svrintn_f32_x(svptrue_b32(), v)));
}

/* base[idx], base[idx + 1], base[idx + 2] per lane */
static inline void hdrg_vf_gather3(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b,
                                   hdrg_vf_t *c)
{
    *a = svld1_gather_u32index_f32(svptrue_b32(), base, idx);
    *b = svld1_gather_u32index_f32(svptrue_b32(), base + 1, idx);
    *c = svld1_gather_u32index_f32(svptrue_b32(), base + 2, idx);
}

/* base[idx], base[idx + 1] per lane */
static inline void hdrg_vf_gather2(const float *base, hdrg_vu_t idx, hdrg_vf_t *a, hdrg_vf_t *b)
{
    *a = svld1_gather_u32index_f32(svptrue_b32(), base, idx);
    *b = svld1_gather_u32index_f32(svptrue_b32(), base + 1, idx);
}

#endif /* HDRG_KERNELS_V_SVE_H */
