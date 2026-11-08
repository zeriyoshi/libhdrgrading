/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "v_sve.h"
#include "vmath.h"

#include "hdrgrading/hdrgrading.h"

/* SVE (provisional): vmath.h over v_sve.h, plus the interleaving of the code planes */

static inline void hdrg_sve_store_rgb(const uint8_t planes[3][HDRG_KERNEL_BLOCK], uint32_t n,
                                      uint8_t *dst)
{
    const svbool_t pg = svwhilelt_b8_u32(0, n);

    svst3_u8(
        pg, dst,
        svcreate3_u8(svld1_u8(pg, planes[0]), svld1_u8(pg, planes[1]), svld1_u8(pg, planes[2])));
}

static void hdrg_sve_pack_srgb(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                               uint32_t row_hash, bool dither, uint8_t *dst)
{
    uint8_t planes[3][HDRG_KERNEL_BLOCK];

    hdrg_v_planes(in, x0, row_hash, dither, false, planes);
    hdrg_sve_store_rgb((const uint8_t (*)[HDRG_KERNEL_BLOCK])planes, n, dst);
}

static void hdrg_sve_pack_pq(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                             uint32_t row_hash, bool dither, uint8_t *dst)
{
    uint8_t planes[3][HDRG_KERNEL_BLOCK];

    hdrg_v_planes(in, x0, row_hash, dither, true, planes);
    hdrg_sve_store_rgb((const uint8_t (*)[HDRG_KERNEL_BLOCK])planes, n, dst);
}

const hdrg_kernels_t hdrg_kernels_sve = {
    .boost = hdrg_v_boost,
    .grade = hdrg_v_grade,
    .lut = hdrg_v_lut,
    .layer = hdrg_v_layer,
    .shoulder = hdrg_v_shoulder,
    .pack_srgb = hdrg_sve_pack_srgb,
    .pack_pq = hdrg_sve_pack_pq,
    .pack_srgb16 = hdrg_v_pack_srgb16,
    .pack_pq16 = hdrg_v_pack_pq16,
    .tier = HDRG_SIMD_SVE,
    .exact = HDRG_V_EXACT,
};
