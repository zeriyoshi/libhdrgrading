/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_BLOCKME_H
#define HDRG_PLUGINS_BLOCKME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/foundation/pool.h"
#include "hdrgrading/filter.h"

/*
 * The block motion estimation of hdrg.interp.mc and hdrg.mctf, in integers
 * (deterministic, the same whatever the threads; the GPU versions of
 * hdrg.interp.mc follow it exactly, src/plugins/interp/wgsl/interp.wgsl):
 * - a frame's pyramid: its luma (BT.709 weights) at half resolution and the
 *   2 x 2 means down to HDRG_BME_COARSE pixels across;
 * - a vector for each 8 x 8 block of each level at a phase between two
 *   frames, coarse to fine: the frames matched symmetrically about it (the
 *   block at q reads the first frame at q - phase v, the second at
 *   q + (1 - phase) v, bilinear at 1/16 pixel; phase 0: the block of the
 *   first frame found in the second), the sum of absolute differences plus
 *   a penalty off the predictor. The coarsest level searches
 *   +-HDRG_BME_RANGE pixels; each finer one starts from the parent block's
 *   vector and its 4 neighbors' and refines by 1, 1/2 (and at the finest
 *   1/4) of its pixels; a 3 x 3 median after each level.
 */
#define HDRG_BME_LEVELS     6u     /* the pyramid's levels at most ... */
#define HDRG_BME_COARSE     320u   /* ... down to a width within this */
#define HDRG_BME_BLOCK      8u     /* a level's blocks: 8 x 8 of its pixels */
#define HDRG_BME_RANGE      6      /* the coarsest level's search: +-6 of its pixels each way */
#define HDRG_BME_QUARTER    4      /* vectors in quarter pixels of their level */
#define HDRG_BME_PREDICTORS 5u     /* a finer level's: the parent block's and its 4 neighbors' */
#define HDRG_BME_KR         13933u /* BT.709 luma weights in 1/65536 */
#define HDRG_BME_KG         46871u
#define HDRG_BME_KB         4732u

/* a quarter pixel off the predictor: 1/2 8-bit code a pixel of a block (257 x 128 x 64) */
#define HDRG_BME_LAMBDA UINT64_C(2105344)

typedef struct {
    int32_t x, y;
} hdrg_bme_vec_t;

typedef struct {
    uint16_t *data;
    uint32_t width, height;
} hdrg_bme_plane_t;

/* a frame's pyramid (its planes in one block) */
typedef struct {
    hdrg_bme_plane_t levels[HDRG_BME_LEVELS];
    void *block;
} hdrg_bme_pyramid_t;

/* the levels of frames of a size, and each level's field of a search */
typedef struct {
    const hdrg_bme_pyramid_t *a, *b; /* the search's frames */
    hdrg_bme_vec_t *fields[HDRG_BME_LEVELS];
    hdrg_bme_vec_t *median; /* the median's scratch (the finest level's blocks) */
    uint32_t sizes[HDRG_BME_LEVELS][2], blocks_w[HDRG_BME_LEVELS], blocks_h[HDRG_BME_LEVELS];
    uint64_t lambda; /* the search's penalty a quarter pixel off the predictor */
    uint32_t width, height, levels, phase;
    void *block; /* the fields' memory */
} hdrg_bme_t;

/* floor((v + 2^(shift - 1)) / 2^shift): rounded, the same for either sign */
static inline int32_t hdrg_bme_round(int64_t v, uint32_t shift)
{
    const int64_t q = v + ((int64_t)1 << (shift - 1u));

    return (int32_t)((q >= 0) ? (q >> shift) : -((-q + ((int64_t)1 << shift) - 1) >> shift));
}

/* the samples of RGB16 rows at (x, y) in 1/16 pixel, bilinear, the edges repeated: x 256 */
static inline void hdrg_bme_rgb(const uint8_t *rows, size_t stride, uint32_t width, uint32_t height,
                                int32_t x, int32_t y, uint32_t out[3])
{
    const uint16_t *r0, *r1;
    const int32_t top_x = (int32_t)(width - 1u) * 16, top_y = (int32_t)(height - 1u) * 16;
    uint32_t x0, y0, x1, y1, fx, fy;
    uint8_t c;

    x = (x < 0) ? 0 : ((x > top_x) ? top_x : x);
    y = (y < 0) ? 0 : ((y > top_y) ? top_y : y);
    x0 = (uint32_t)x >> 4u;
    y0 = (uint32_t)y >> 4u;
    fx = (uint32_t)x & 15u;
    fy = (uint32_t)y & 15u;
    x1 = (x0 + 1u < width) ? x0 + 1u : x0;
    y1 = (y0 + 1u < height) ? y0 + 1u : y0;
    r0 = (const uint16_t *)(const void *)(rows + (size_t)y0 * stride);
    r1 = (const uint16_t *)(const void *)(rows + (size_t)y1 * stride);

    for (c = 0; c < 3u; c++) {
        out[c] = (16u - fx) * (16u - fy) * r0[x0 * 3u + c] + fx * (16u - fy) * r0[x1 * 3u + c] +
                 (16u - fx) * fy * r1[x0 * 3u + c] + fx * fy * r1[x1 * 3u + c];
    }
}

/*
 * fn over rows [0, rows) in chunks of grain rows on the context's threads
 * (the API table's run_parallel: worker below run_workers), returning when
 * all ran; not from within run_parallel's work
 */
extern void hdrg_bme_rows(const hdrg_plugin_api_t *api, uint32_t rows, uint32_t grain,
                          hdrg_pool_rows_fn_t fn, void *arg);
/*
 * the horizontal half of hdrg_bme_vector at pixel x from a row's columns
 * (hdrg_bme_vector_row): the same vector
 */
static inline void hdrg_bme_vector_at(const int32_t *columns, uint32_t bw, uint32_t x, int32_t *vx,
                                      int32_t *vy)
{
    uint32_t bx0, bx1, wx;
    int32_t px;

    px = (int32_t)x * 16 - 120;
    px = (px < 0) ? 0 : px;
    bx0 = (uint32_t)px >> 8u;
    wx = (uint32_t)px & 255u;

    if (bx0 + 1u >= bw) {
        bx0 = bw - 1u;
        wx = 0;
    }

    bx1 = (bx0 + 1u < bw) ? bx0 + 1u : bx0;
    *vx = hdrg_bme_round(
        ((int64_t)(256u - wx) * columns[2u * bx0] + (int64_t)wx * columns[2u * bx1]) * 8, 16u);
    *vy = hdrg_bme_round(
        ((int64_t)(256u - wx) * columns[2u * bx0 + 1u] + (int64_t)wx * columns[2u * bx1 + 1u]) * 8,
        16u);
}

/* the levels for width x height (sizes, blocks and levels; no fields) */
extern void hdrg_bme_shape(hdrg_bme_t *bme, uint32_t width, uint32_t height);
/* the levels for width x height and the fields (kept while the size stays) */
extern hdrg_status_t hdrg_bme_fit(hdrg_bme_t *bme, uint32_t width, uint32_t height,
                                  const hdrg_plugin_api_t *api);
extern void hdrg_bme_free(hdrg_bme_t *bme, const hdrg_plugin_api_t *api);
/* a pyramid's planes for bme's levels */
extern hdrg_status_t hdrg_bme_pyramid_fit(const hdrg_bme_t *bme, hdrg_bme_pyramid_t *pyramid,
                                          const hdrg_plugin_api_t *api);
extern void hdrg_bme_pyramid_free(hdrg_bme_pyramid_t *pyramid, const hdrg_plugin_api_t *api);
/* the pyramid of a frame of RGB16 rows of bme's size */
extern void hdrg_bme_pyramid(const hdrg_bme_t *bme, hdrg_bme_pyramid_t *pyramid,
                             const uint8_t *rows, size_t stride, const hdrg_plugin_api_t *api);
/*
 * the fields of a and b at phase (1/65536), coarse to fine, lambda the
 * penalty a quarter pixel off the predictor (hdrg.interp.mc's:
 * HDRG_BME_LAMBDA)
 */
extern void hdrg_bme_search(hdrg_bme_t *bme, const hdrg_bme_pyramid_t *a,
                            const hdrg_bme_pyramid_t *b, uint32_t phase, uint64_t lambda,
                            const hdrg_plugin_api_t *api);
/* block (bx, by)'s cost at level for v about pred (the search's frames and phase) */
extern uint64_t hdrg_bme_cost(const hdrg_bme_t *bme, uint32_t level, uint32_t bx, uint32_t by,
                              hdrg_bme_vec_t v, hdrg_bme_vec_t pred);
/*
 * a finest field's (bw x bh blocks) vector at full-resolution pixel (x, y):
 * 1/16 pixel, bilinear between the block centers
 */
extern void hdrg_bme_vector(const hdrg_bme_vec_t *field, uint32_t bw, uint32_t bh, uint32_t x,
                            uint32_t y, int32_t *vx, int32_t *vy);
/*
 * the vertical half of hdrg_bme_vector for row y into columns (2 bw: each
 * block column's x and y between the block centers above and below, x 256)
 */
extern void hdrg_bme_vector_row(const hdrg_bme_vec_t *field, uint32_t bw, uint32_t bh, uint32_t y,
                                int32_t *columns);
/*
 * The motion as HDRG_SD_MOTION (sidedata.h): count fields of the finest
 * level's blocks_w x blocks_h blocks (2 x HDRG_BME_BLOCK pixels, the luma
 * at half size, the INTERNAL source), the other frames at offsets, at
 * phase: its bytes, the headers into side, and block i of field k
 */
extern size_t hdrg_bme_motion_size(uint32_t blocks_w, uint32_t blocks_h, uint32_t count);
extern void hdrg_bme_motion_head(uint32_t blocks_w, uint32_t blocks_h, uint32_t count,
                                 const int32_t *offsets, uint32_t phase, uint8_t *side);
extern void hdrg_bme_motion_block(uint32_t blocks_w, uint32_t blocks_h, uint32_t k, size_t i,
                                  const hdrg_sd_motion_block_t *record, uint8_t *side);

#endif /* HDRG_PLUGINS_BLOCKME_H */
