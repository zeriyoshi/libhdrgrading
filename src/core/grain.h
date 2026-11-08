/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_GRAIN_H
#define HDRG_CORE_GRAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "exec.h"
#include "hdrgrading/filter.h"
#include "image.h"
#include "noise.h"

#define HDRG_GRAIN_RADIUS 3u                            /* the filter's taps each way ... */
#define HDRG_GRAIN_TAPS   (2u * HDRG_GRAIN_RADIUS + 1u) /* ... and in all */
#define HDRG_GRAIN_LEVELS 256u                          /* the scales' source Y' levels */
#define HDRG_GRAIN_ROWS   (3u * HDRG_GRAIN_TAPS) /* a worker's filtered rows: each plane's taps */
#define HDRG_GRAIN_KEY_HI UINT32_C(0x4752414E)   /* the grain's hashes' high key word */
#define HDRG_GRAIN_WORDS                                                                           \
    (6u + HDRG_GRAIN_TAPS + 3u * HDRG_GRAIN_LEVELS) /* hdrg_grain_words: the GPU's layout */
#define HDRG_GRAIN_FILM_BYTES                                                                      \
    (sizeof(hdrg_sd_film_grain_t) + sizeof(hdrg_sd_film_grain_av1_t)) /* HDRG_SD_FILM_GRAIN's */

/*
 * The grain of a frame to put back (grain_policy RESYNTH: the one its
 * PREFILTER stage removed; a codec's parameters, HDRG_SD_FILM_GRAIN: the one
 * its decoder left out): the standard deviation of its Y', Cb and Cr over
 * the full scale by the source's Y' level (Cb and Cr the source's
 * coefficients'), and the lag-1 correlation of its Y'. The removed one is
 * the difference of the frame into the chain's first stage with a time
 * window (the temporal noise reduction's; without one, into the chain) and
 * out of the chain (RGB16), measured on the noise estimate's tiles
 * (hdrg_noise_grid): Y' by the output's level in eighths, its correlation
 * from the variances of its first and second differences, each the mean of
 * the middle half of the tiles (those whose detail the stage took count
 * less).
 */
typedef struct {
    float sigma[3][HDRG_GRAIN_LEVELS]; /* Y', Cb, Cr by the source's 8-bit Y' level */
    float rho;
    bool valid; /* some grain (a frame without its tiles, a chain without the stage: none) */
} hdrg_grain_stats_t;

/*
 * Film grain resynthesized in the output (grain_policy RESYNTH): white noise of the counter hash
 * (the frame's key, the row, the column; Y', Cb and Cr apart) through a separable Gaussian of the
 * removed grain's lag-1 correlation, at its level for each Y' level as the
 * source's SDR rendition showed it in PQ (the PQ code's slope there at SDR
 * white): the grain the source had, not amplified by the expansion. Added
 * to pass C's PQ R'G'B' rows, Cb and Cr through BT.2020's matrix. Integers
 * from the plan on, every product within 32 bits (the GPU's apply, apply.wgsl,
 * gives the same codes): the same bytes whatever the threads and the bands.
 */
typedef struct {
    int32_t taps[HDRG_GRAIN_TAPS];       /* Q8, their squares summing to about 1 */
    int32_t scale[3][HDRG_GRAIN_LEVELS]; /* Y', Cb, Cr: 16-bit PQ codes of a unit of the
                                            filtered noise (Q12), by the source's Y' */
    uint32_t luma[3];                    /* the source's Y' coefficients (Q15): the level */
    uint32_t key;                        /* the frame's */
} hdrg_grain_t;

/* a mosaic of the grid's windows (hdrg_noise_estimate_mosaic's layout): its samples */
extern size_t hdrg_grain_windows(const hdrg_noise_grid_t *grid);
/* the grid's windows of a frame (RGB16, rows of stride bytes) into mosaic, as the GPU's */
extern void hdrg_grain_gather(const hdrg_exec_t *exec, const hdrg_noise_grid_t *grid,
                              const uint16_t *rgb, size_t stride, uint16_t *mosaic);
/*
 * The grain removed from a frame: the grid's windows of it before and
 * after gathered into mosaics (hdrg_grain_gather, src/core/gpu_gather.c),
 * coeffs the source's
 */
extern hdrg_status_t hdrg_grain_measure(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                        const uint16_t *before, const uint16_t *after,
                                        const hdrg_noise_grid_t *grid, const float coeffs[3],
                                        hdrg_grain_stats_t *out);
/* HDRG_SD_FILM_GRAIN's size bytes at data as version 1 has them (its kinds, its ranges) */
extern bool hdrg_grain_film_check(const void *data, size_t size);
/*
 * The grain of film parameters checked (HDRG_GRAIN_FILM_BYTES at data): AV1's
 * scaling functions over the standard deviation of their templates' grain
 * (the specification's auto-regression run on the counter hash's white
 * noise of its Gaussian sequence's), Cb and Cr at the neutral chroma
 */
extern void hdrg_grain_film(const void *data, hdrg_grain_stats_t *out);
/* a frame's key: its place in the stream, and between it and the next (1/65536; 0: itself) */
extern uint32_t hdrg_grain_key_of(uint32_t index, uint32_t phase);
/* the plane's low key word (the hash's; its high word HDRG_GRAIN_KEY_HI) */
extern uint32_t hdrg_grain_plane_key(const hdrg_grain_t *grain, uint32_t plane);
/*
 * The grain of a frame of stats on src (its EOTF and coefficients) shown
 * in SDR at sdr_white_nits, amount times the removed one, keyed by key
 * (hdrg_grain_key_of); false: none (not measured, amount 0)
 */
extern bool hdrg_grain_plan(const hdrg_grain_stats_t *stats, const hdrg_source_t *src,
                            float sdr_white_nits, float amount, uint32_t key, hdrg_grain_t *out);
/*
 * The grain as the GPU's apply reads it (apply.wgsl, HDRG_GRAIN_WORDS):
 * each plane's low key word, the luma coefficients, the taps, the scales
 */
extern void hdrg_grain_words(const hdrg_grain_t *grain, uint32_t *words);
/*
 * Output row y (width pixels of 16-bit PQ R'G'B', or 8-bit: sample_bytes)
 * with the grain added at the levels of the source's row src_row; rows: a
 * worker's HDRG_GRAIN_ROWS x width words (filled for its run's first row,
 * else a row more), scratch: width + 2 HDRG_GRAIN_RADIUS words
 */
extern void hdrg_grain_row(const hdrg_grain_t *grain, const hdrg_source_t *src,
                           const uint8_t *src_row, uint32_t y, bool first, int32_t *rows,
                           int32_t *scratch, uint8_t *dst, uint8_t sample_bytes);

#endif /* HDRG_CORE_GRAIN_H */
