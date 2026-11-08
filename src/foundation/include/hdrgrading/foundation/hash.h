/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_HASH_H
#define HDRG_FOUNDATION_HASH_H

#include <stdint.h>

/*
 * Counter-based hash for dithering and noise. A sample is a pure
 * function of (key, x, y, sample), so tiles, threads, SIMD lanes and shader invocations compute
 * it independently and in any order. Only 32-bit integer operations are used, so the SIMD tiers
 * and WGSL can reproduce it bit for bit.
 *
 * The 32-bit words are absorbed one at a time, state = lowbias32(state ^ word * W), where
 * lowbias32 is Chris Wellons' two-round xorshift-multiply finalizer (hash-prospector, exact
 * avalanche bias 0.1735, slightly better than murmur3 fmix32) and W is an odd constant per word
 * position (the
 * xxHash32 primes), which turns the +1 steps of neighbouring coordinates into dense bit
 * differences before the finalizer. Order: key low word, y, key high word, x, sample. A kernel
 * hoists the row (hdrg_hash_row) and pixel (hdrg_hash_pixel) stages out of its inner loops; for a
 * fixed key, the row state is a bijection of y and the pixel state a bijection of x.
 */

#define HDRG_HASH_IV     UINT32_C(0x6a09e667)
#define HDRG_HASH_W_KEY0 UINT32_C(0x9e3779b1)
#define HDRG_HASH_W_Y    UINT32_C(0x85ebca77)
#define HDRG_HASH_W_KEY1 UINT32_C(0xc2b2ae3d)
#define HDRG_HASH_W_X    UINT32_C(0x27d4eb2f)
#define HDRG_HASH_W_S    UINT32_C(0x165667b1)

/* lowbias32 (bijective) */
static inline uint32_t hdrg_hash_mix32(uint32_t h)
{
    h ^= h >> 16;
    h *= UINT32_C(0x7feb352d);
    h ^= h >> 15;
    h *= UINT32_C(0x846ca68b);
    h ^= h >> 16;

    return h;
}

static inline uint32_t hdrg_hash_row(uint64_t key, uint32_t y)
{
    uint32_t h;

    h = hdrg_hash_mix32(HDRG_HASH_IV ^ ((uint32_t)key * HDRG_HASH_W_KEY0));
    h = hdrg_hash_mix32(h ^ (y * HDRG_HASH_W_Y));

    return hdrg_hash_mix32(h ^ ((uint32_t)(key >> 32) * HDRG_HASH_W_KEY1));
}

static inline uint32_t hdrg_hash_pixel(uint32_t row, uint32_t x)
{
    return hdrg_hash_mix32(row ^ (x * HDRG_HASH_W_X));
}

static inline uint32_t hdrg_hash_sample(uint32_t pixel, uint32_t sample)
{
    return hdrg_hash_mix32(pixel ^ (sample * HDRG_HASH_W_S));
}

static inline uint32_t hdrg_hash_u32(uint64_t key, uint32_t x, uint32_t y, uint32_t sample)
{
    return hdrg_hash_sample(hdrg_hash_pixel(hdrg_hash_row(key, y), x), sample);
}

/* Uniform in [0, 1 - 2^-24] from the top 24 bits (exact: k * 2^-24 for an integer k < 2^24). */
static inline float hdrg_hash_unit(uint32_t h)
{
    return (float)(h >> 8) * 0x1p-24f;
}

/*
 * Triangular in [-(1 - 2^-24), 1 - 2^-24] (inside (-1, 1)) from two uniforms: u_a - u_b, which is
 * u_a + (1 - u_b) - 1, computed exactly. Mean 0, variance 1/6 (TPDF dither scaled to +-1 LSB).
 */
static inline float hdrg_hash_tri(uint32_t a, uint32_t b)
{
    return ((float)(a >> 8) - (float)(b >> 8)) * 0x1p-24f;
}

static inline float hdrg_hash_uniform(uint64_t key, uint32_t x, uint32_t y, uint32_t sample)
{
    return hdrg_hash_unit(hdrg_hash_u32(key, x, y, sample));
}

/* Consumes sample words 2 * sample and 2 * sample + 1 (sample < 2^31). */
static inline float hdrg_hash_triangular(uint64_t key, uint32_t x, uint32_t y, uint32_t sample)
{
    const uint32_t pixel = hdrg_hash_pixel(hdrg_hash_row(key, y), x);

    return hdrg_hash_tri(hdrg_hash_sample(pixel, sample * 2u),
                         hdrg_hash_sample(pixel, sample * 2u + 1u));
}

#endif /* HDRG_FOUNDATION_HASH_H */
