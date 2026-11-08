/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "v_neon.h"
#include "vmath.h"

#include "hdrgrading/hdrgrading.h"

/* AArch64 Advanced SIMD: vmath.h over v_neon.h, plus the interleaving of the code planes */

static inline void hdrg_neon_store_rgb(const uint8_t planes[3][HDRG_KERNEL_BLOCK], uint32_t n,
                                       uint8_t *dst)
{
    uint8x16x3_t rgb;
    uint32_t i;
    uint8_t c;

    if (n == HDRG_KERNEL_BLOCK) {
        for (c = 0; c < 3; c++) {
            rgb.val[c] = vld1q_u8(planes[c]);
        }

        vst3q_u8(dst, rgb);

        return;
    }

    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            dst[(size_t)i * 3 + c] = planes[c][i];
        }
    }
}

static void hdrg_neon_pack_srgb(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                                uint32_t row_hash, bool dither, uint8_t *dst)
{
    uint8_t planes[3][HDRG_KERNEL_BLOCK];

    hdrg_v_planes(in, x0, row_hash, dither, false, planes);
    hdrg_neon_store_rgb((const uint8_t (*)[HDRG_KERNEL_BLOCK])planes, n, dst);
}

static void hdrg_neon_pack_pq(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                              uint32_t row_hash, bool dither, uint8_t *dst)
{
    uint8_t planes[3][HDRG_KERNEL_BLOCK];

    hdrg_v_planes(in, x0, row_hash, dither, true, planes);
    hdrg_neon_store_rgb((const uint8_t (*)[HDRG_KERNEL_BLOCK])planes, n, dst);
}

const hdrg_kernels_t hdrg_kernels_neon = {
    .boost = hdrg_v_boost,
    .grade = hdrg_v_grade,
    .lut = hdrg_v_lut,
    .layer = hdrg_v_layer,
    .shoulder = hdrg_v_shoulder,
    .pack_srgb = hdrg_neon_pack_srgb,
    .pack_pq = hdrg_neon_pack_pq,
    .pack_srgb16 = hdrg_v_pack_srgb16,
    .pack_pq16 = hdrg_v_pack_pq16,
    .tier = HDRG_SIMD_NEON,
    .exact = HDRG_V_EXACT,
};
