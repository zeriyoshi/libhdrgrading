/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_NOISE_H
#define HDRG_CORE_NOISE_H

#include <stdbool.h>
#include <stdint.h>

#include "hdrgrading/filter.h"
#include "image.h"

#define HDRG_NOISE_BINS   8u  /* Y' bins of the noise level function over [0, 1] */
#define HDRG_NOISE_TILE   16u /* a tile's side */
#define HDRG_NOISE_WINDOW 18u /* a tile's window: the tile and the kernel's border of 1 */

/*
 * Noise of a source (the content of HDRG_SD_NOISE).
 * Immerkær's estimator (the 3x3 kernel [1 -2 1]^T [1 -2 1], blind to
 * planes and to straight horizontal / vertical edges) on a fixed grid of
 * 16 x 16 tiles at full resolution; a level is the mean of its quietest
 * tiles, so texture and edges in the others do not count. Tiles touching
 * code 0 or the top code (clipped noise) are left out. Levels are standard
 * deviations of the code values over the full scale (an 8-bit code is
 * 1/255); Y' / Cb / Cr are those of the source's luminance coefficients on
 * its code values. Deterministic and independent of the worker count.
 */
typedef struct {
    float sigma;                    /* Y' over every level; < 0 = no data (frame too small) */
    float sigma_y[HDRG_NOISE_BINS]; /* Y' of the tiles whose mean Y' is in each bin; < 0 =
                                       too few tiles */
    float sigma_cb;
    float sigma_cr;
    float blockiness; /* across the 8-pixel grid over inside it, mean |step| of Y' on the
                         quieter half of the tiles, steps of edges (over 12 8-bit codes)
                         left out: 1 = none */
    float block_step; /* the mean |step| the grid adds there (across minus inside) */
    float ringing;    /* in the grid's blocks with an edge, the mean |Y' - its 3x3 mean| away
                         from the edge over the same in the blocks without one: 1 = none */
    float ring_level; /* the first mean minus the second (0 = none) */
    uint32_t tiles;   /* tiles measured (unclipped) */
} hdrg_noise_t;

/*
 * The tiles an estimate reads of a frame (hdrg_noise_grid): nx x ny of them,
 * tile (i, j) at tile indices (first_x + i step, first_y + j step), its
 * pixels [16 x, 16 x + 16) on each axis
 */
typedef struct {
    uint32_t nx, ny;
    uint32_t step;
    uint32_t first_x, first_y;
} hdrg_noise_grid_t;

/* a level in 8-bit codes, -1 without data */
static inline float hdrg_noise_codes(float sigma)
{
    return (sigma < 0.0f) ? -1.0f : sigma * 255.0f;
}

/* the estimate as the side data HDRG_SD_NOISE (HDRG_SD_NOISE_VERSION) */
static inline void hdrg_noise_side_data(const hdrg_noise_t *noise, float out[HDRG_SD_NOISE_FLOATS])
{
    uint8_t b;

    out[0] = noise->sigma;

    for (b = 0; b < HDRG_NOISE_BINS; b++) {
        out[1u + b] = noise->sigma_y[b];
    }

    out[9] = noise->sigma_cb;
    out[10] = noise->sigma_cr;
    out[11] = noise->blockiness;
    out[12] = noise->block_step;
    out[13] = noise->ringing;
    out[14] = noise->ring_level;
}

/* the grid of a width x height frame's estimate; false: none (too small, no data) */
extern bool hdrg_noise_grid(uint32_t width, uint32_t height, hdrg_noise_grid_t *grid);
extern hdrg_status_t hdrg_noise_estimate(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                         const hdrg_source_t *src, hdrg_noise_t *out);
/*
 * The same estimate from the grid's windows gathered into mosaic (the GPU's,
 * src/core/gpu_gather.c): window r, HDRG_NOISE_WINDOW square, at
 * HDRG_NOISE_WINDOW x (r % nx, r / nx); the other fields of mosaic the
 * frame's (its codes and coefficients)
 */
extern hdrg_status_t hdrg_noise_estimate_mosaic(const hdrg_allocator_t *alloc,
                                                const hdrg_exec_t *exec,
                                                const hdrg_source_t *mosaic,
                                                const hdrg_noise_grid_t *grid, hdrg_noise_t *out);
/*
 * the noise level function at Y' level y: the bins with data interpolated
 * (held past the outer ones), sigma without any, 0 without data
 */
extern float hdrg_noise_at(const hdrg_noise_t *noise, float y);

#endif /* HDRG_CORE_NOISE_H */
