/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "v_avx512.h"
#include "vmath.h"
#include "x86_rgb.h"

#include "hdrgrading/hdrgrading.h"

/* AVX-512 F / DQ / BW / VL (provisional): vmath.h over v_avx512.h, plus the interleaving of the
 * planes */

static void hdrg_avx512_pack_srgb(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                                  uint32_t row_hash, bool dither, uint8_t *dst)
{
    uint8_t planes[3][HDRG_KERNEL_BLOCK];

    hdrg_v_planes(in, x0, row_hash, dither, false, planes);
    hdrg_x86_store_rgb((const uint8_t (*)[HDRG_KERNEL_BLOCK])planes, n, dst);
}

static void hdrg_avx512_pack_pq(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                                uint32_t row_hash, bool dither, uint8_t *dst)
{
    uint8_t planes[3][HDRG_KERNEL_BLOCK];

    hdrg_v_planes(in, x0, row_hash, dither, true, planes);
    hdrg_x86_store_rgb((const uint8_t (*)[HDRG_KERNEL_BLOCK])planes, n, dst);
}

const hdrg_kernels_t hdrg_kernels_avx512 = {
    .boost = hdrg_v_boost,
    .grade = hdrg_v_grade,
    .lut = hdrg_v_lut,
    .layer = hdrg_v_layer,
    .shoulder = hdrg_v_shoulder,
    .pack_srgb = hdrg_avx512_pack_srgb,
    .pack_pq = hdrg_avx512_pack_pq,
    .pack_srgb16 = hdrg_v_pack_srgb16,
    .pack_pq16 = hdrg_v_pack_pq16,
    .tier = HDRG_SIMD_AVX512,
    .exact = HDRG_V_EXACT,
};
