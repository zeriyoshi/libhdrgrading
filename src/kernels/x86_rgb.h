/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_KERNELS_X86_RGB_H
#define HDRG_KERNELS_X86_RGB_H

#include <stdint.h>
#include <string.h>
#include <tmmintrin.h>

#include "kernels.h"

/* pshufb masks interleaving 16 R, G, B bytes into 48 RGB bytes: [output vector][channel] */
static const int8_t hdrg_x86_rgb_masks[3][3][16] = {
    {
        {0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, 4, -1, -1, 5},
        {-1, 0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, 4, -1, -1},
        {-1, -1, 0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, 4, -1},
    },
    {
        {-1, -1, 6, -1, -1, 7, -1, -1, 8, -1, -1, 9, -1, -1, 10, -1},
        {5, -1, -1, 6, -1, -1, 7, -1, -1, 8, -1, -1, 9, -1, -1, 10},
        {-1, 5, -1, -1, 6, -1, -1, 7, -1, -1, 8, -1, -1, 9, -1, -1},
    },
    {
        {-1, 11, -1, -1, 12, -1, -1, 13, -1, -1, 14, -1, -1, 15, -1, -1},
        {-1, -1, 11, -1, -1, 12, -1, -1, 13, -1, -1, 14, -1, -1, 15, -1},
        {10, -1, -1, 11, -1, -1, 12, -1, -1, 13, -1, -1, 14, -1, -1, 15},
    },
};

/* n pixels of the R, G, B code planes, interleaved at dst (SSSE3: every x86 tier) */
static inline void hdrg_x86_store_rgb(const uint8_t planes[3][HDRG_KERNEL_BLOCK], uint32_t n,
                                      uint8_t *dst)
{
    __m128i plane[3], rgb[3];
    uint8_t c, k, bytes[3 * HDRG_KERNEL_BLOCK];

    for (c = 0; c < 3; c++) {
        plane[c] = _mm_loadu_si128((const __m128i *)planes[c]);
    }

    for (k = 0; k < 3; k++) {
        rgb[k] = _mm_or_si128(
            _mm_or_si128(_mm_shuffle_epi8(
                             plane[0], _mm_loadu_si128((const __m128i *)hdrg_x86_rgb_masks[k][0])),
                         _mm_shuffle_epi8(
                             plane[1], _mm_loadu_si128((const __m128i *)hdrg_x86_rgb_masks[k][1]))),
            _mm_shuffle_epi8(plane[2], _mm_loadu_si128((const __m128i *)hdrg_x86_rgb_masks[k][2])));
    }

    if (n == HDRG_KERNEL_BLOCK) {
        for (k = 0; k < 3; k++) {
            _mm_storeu_si128((__m128i *)(dst + (size_t)k * 16u), rgb[k]);
        }

        return;
    }

    for (k = 0; k < 3; k++) {
        _mm_storeu_si128((__m128i *)(bytes + (size_t)k * 16u), rgb[k]);
    }

    memcpy(dst, bytes, (size_t)n * 3u);
}

#endif /* HDRG_KERNELS_X86_RGB_H */
