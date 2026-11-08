/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "v_wasm128.h"
#include "vmath.h"

#include "hdrgrading/hdrgrading.h"

/* WebAssembly SIMD128: vmath.h over v_wasm128.h, plus the interleaving of the code planes */

/* n pixels of the R, G, B code planes, interleaved at dst: R and G first, then B into the gaps */
static inline void hdrg_wasm_store_rgb(const uint8_t planes[3][HDRG_KERNEL_BLOCK], uint32_t n,
                                       uint8_t *dst)
{
    v128_t r, g, b, rgb[3];
    uint8_t k, bytes[3 * HDRG_KERNEL_BLOCK];

    r = wasm_v128_load(planes[0]);
    g = wasm_v128_load(planes[1]);
    b = wasm_v128_load(planes[2]);

    rgb[0] = wasm_i8x16_shuffle(
        wasm_i8x16_shuffle(r, g, 0, 16, 0, 1, 17, 0, 2, 18, 0, 3, 19, 0, 4, 20, 0, 5), b, 0, 1, 16,
        3, 4, 17, 6, 7, 18, 9, 10, 19, 12, 13, 20, 15);
    rgb[1] = wasm_i8x16_shuffle(
        wasm_i8x16_shuffle(r, g, 21, 0, 6, 22, 0, 7, 23, 0, 8, 24, 0, 9, 25, 0, 10, 26), b, 0, 21,
        2, 3, 22, 5, 6, 23, 8, 9, 24, 11, 12, 25, 14, 15);
    rgb[2] = wasm_i8x16_shuffle(
        wasm_i8x16_shuffle(r, g, 0, 11, 27, 0, 12, 28, 0, 13, 29, 0, 14, 30, 0, 15, 31, 0), b, 26,
        1, 2, 27, 4, 5, 28, 7, 8, 29, 10, 11, 30, 13, 14, 31);

    if (n == HDRG_KERNEL_BLOCK) {
        for (k = 0; k < 3; k++) {
            wasm_v128_store(dst + (size_t)k * 16u, rgb[k]);
        }

        return;
    }

    for (k = 0; k < 3; k++) {
        wasm_v128_store(bytes + (size_t)k * 16u, rgb[k]);
    }

    memcpy(dst, bytes, (size_t)n * 3u);
}

static void hdrg_wasm_pack_srgb(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                                uint32_t row_hash, bool dither, uint8_t *dst)
{
    uint8_t planes[3][HDRG_KERNEL_BLOCK];

    hdrg_v_planes(in, x0, row_hash, dither, false, planes);
    hdrg_wasm_store_rgb((const uint8_t (*)[HDRG_KERNEL_BLOCK])planes, n, dst);
}

static void hdrg_wasm_pack_pq(const float in[3][HDRG_KERNEL_BLOCK], uint32_t n, uint32_t x0,
                              uint32_t row_hash, bool dither, uint8_t *dst)
{
    uint8_t planes[3][HDRG_KERNEL_BLOCK];

    hdrg_v_planes(in, x0, row_hash, dither, true, planes);
    hdrg_wasm_store_rgb((const uint8_t (*)[HDRG_KERNEL_BLOCK])planes, n, dst);
}

const hdrg_kernels_t hdrg_kernels_wasm128 = {
    .boost = hdrg_v_boost,
    .grade = hdrg_v_grade,
    .lut = hdrg_v_lut,
    .layer = hdrg_v_layer,
    .shoulder = hdrg_v_shoulder,
    .pack_srgb = hdrg_wasm_pack_srgb,
    .pack_pq = hdrg_wasm_pack_pq,
    .pack_srgb16 = hdrg_v_pack_srgb16,
    .pack_pq16 = hdrg_v_pack_pq16,
    .tier = HDRG_SIMD_WASM128,
    .exact = HDRG_V_EXACT,
};
