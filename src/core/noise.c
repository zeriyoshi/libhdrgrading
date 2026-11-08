/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "noise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HDRG_NOISE_SPAN      HDRG_NOISE_WINDOW
#define HDRG_NOISE_MAX_TILES 4096u
#define HDRG_NOISE_MIN_TILES 8u
#define HDRG_NOISE_QUIET     0.3f /* the fraction of the quietest tiles a level averages */
/*
 * Immerkær: sqrt(pi / 2) / 6 per |kernel response|, over the mean of the
 * quietest 30% of 16 x 16 tiles relative to all of them under white
 * Gaussian noise (0.90, measured), so pure noise reads its sigma
 */
#define HDRG_NOISE_SCALE     (0.20888568f / 0.90f)
#define HDRG_NOISE_GRID      8u /* block grid of the blockiness: 8-pixel DCT blocks */
#define HDRG_NOISE_BLOCK_MAX 16.0f
#define HDRG_NOISE_STEP_MIN  0.00001f
#define HDRG_NOISE_STEP_MAX                                                                        \
    (12.0f / 255.0f) /* larger steps are edges (UI on the grid), not blocks */
#define HDRG_NOISE_RING_EDGE (32.0f / 255.0f) /* a block's Y' range from which it has an edge */

typedef struct {
    float sigma[3]; /* Y', Cb, Cr */
    float level;    /* mean Y' */
    float edge;     /* sum of |step| of Y' across the block grid */
    float inner;    /* inside it */
    float ring;     /* sum of |Y' - its 3x3 mean| away from the edge in blocks with one */
    float flat;     /* of every sample in blocks without one */
    uint16_t edges; /* steps counted in edge */
    uint16_t inners;
    uint16_t rings; /* samples counted in ring */
    uint16_t flats;
    bool clipped;
} hdrg_noise_tile_t;

typedef struct {
    const hdrg_source_t *src;
    const hdrg_noise_grid_t *grid;
    hdrg_noise_tile_t *tiles;
    bool mosaic; /* src holds the grid's windows (hdrg_noise_estimate_mosaic) */
} hdrg_noise_job_t;

static int hdrg_noise_compare(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

/* the mean of the quietest HDRG_NOISE_QUIET of n values, which it sorts */
static float hdrg_noise_level(float *values, uint32_t n)
{
    uint32_t quiet, i;
    double sum;

    qsort(values, n, sizeof(float), hdrg_noise_compare);

    quiet = HDRG_MAX((uint32_t)((float)n * HDRG_NOISE_QUIET), 1u);
    sum = 0.0;

    for (i = 0; i < quiet; i++) {
        sum += (double)values[i];
    }

    return (float)(sum / (double)quiet);
}

/*
 * The ringing sums of a tile's four 8 x 8 blocks (luma: the tile's window),
 * |Y' - its 3 x 3 mean| of the samples off the tile's outer ring: in a block
 * whose Y' range reaches HDRG_NOISE_RING_EDGE, of those whose 5 x 5 lies on
 * one side of the block's mid level (two pixels away from its edge: not the
 * anti-aliasing of a clean edge); in the others of every one
 */
static void hdrg_noise_ring(const float luma[HDRG_NOISE_SPAN][HDRG_NOISE_SPAN],
                            hdrg_noise_tile_t *out)
{
    uint8_t bx, by, x, y, dx, dy, above;
    bool edged;
    double ring, flat;
    float lo, hi, mid, sum, dev, v;

    ring = 0.0;
    flat = 0.0;
    out->rings = 0;
    out->flats = 0;

    for (by = 0; by < 2u; by++) {
        for (bx = 0; bx < 2u; bx++) {
            lo = luma[1u + by * 8u][1u + bx * 8u];
            hi = lo;

            for (y = 0; y < 8u; y++) {
                for (x = 0; x < 8u; x++) {
                    v = luma[1u + by * 8u + y][1u + bx * 8u + x];
                    lo = HDRG_MIN(lo, v);
                    hi = HDRG_MAX(hi, v);
                }
            }

            edged = hi - lo >= HDRG_NOISE_RING_EDGE;
            mid = (hi + lo) * 0.5f;

            for (y = (by == 0) ? 1u : 0u; y < ((by == 0) ? 8u : 7u); y++) {
                for (x = (bx == 0) ? 1u : 0u; x < ((bx == 0) ? 8u : 7u); x++) {
                    sum = 0.0f;
                    above = 0;

                    for (dy = 0; dy < 5u; dy++) {
                        for (dx = 0; dx < 5u; dx++) {
                            v = luma[by * 8u + y + dy - 1u][bx * 8u + x + dx - 1u];
                            sum += (dy >= 1u && dy <= 3u && dx >= 1u && dx <= 3u) ? v : 0.0f;
                            above = (uint8_t)(above + ((v >= mid) ? 1u : 0u));
                        }
                    }

                    dev = fabsf(luma[1u + by * 8u + y][1u + bx * 8u + x] - sum / 9.0f);

                    if (!edged) {
                        flat += (double)dev;
                        out->flats++;
                    } else if (above == 0 || above == 25u) {
                        ring += (double)dev;
                        out->rings++;
                    }
                }
            }
        }
    }

    out->ring = (float)ring;
    out->flat = (float)flat;
}

/*
 * the tile at (x0, y0) of the frame, its interior [x0, x0 + 16) x [y0,
 * y0 + 16) and the window one pixel more read from (sx, sy) of src
 */
static void hdrg_noise_tile(const hdrg_source_t *src, uint32_t sx, uint32_t sy, uint32_t x0,
                            uint32_t y0, hdrg_noise_tile_t *out)
{
    const uint32_t clip = src->wide ? 65535u : 255u;
    const uint8_t *row;
    const float *k = src->coeffs, top = (float)clip;
    uint32_t x, y, codes[3];
    uint8_t c;
    int32_t v[3][HDRG_NOISE_SPAN][HDRG_NOISE_SPAN], h[3][HDRG_NOISE_SPAN][HDRG_NOISE_TILE], n[3];
    bool clipped;
    double sum[3], level, edge, inner;
    float luma[HDRG_NOISE_SPAN][HDRG_NOISE_SPAN], ny, step;

    clipped = false;

    for (y = 0; y < HDRG_NOISE_SPAN; y++) {
        row = hdrg_source_row(src, sy + y);

        for (x = 0; x < HDRG_NOISE_SPAN; x++) {
            hdrg_source_codes(src, row, sx + x, codes);

            for (c = 0; c < 3; c++) {
                v[c][y][x] = (int32_t)codes[c];
                clipped = clipped || codes[c] == 0 || codes[c] == clip;
            }

            luma[y][x] =
                (k[0] * (float)codes[0] + k[1] * (float)codes[1] + k[2] * (float)codes[2]) / top;
        }
    }

    for (c = 0; c < 3; c++) {
        for (y = 0; y < HDRG_NOISE_SPAN; y++) {
            for (x = 0; x < HDRG_NOISE_TILE; x++) {
                h[c][y][x] = v[c][y][x] - 2 * v[c][y][x + 1u] + v[c][y][x + 2u];
            }
        }
    }

    memset(sum, 0, sizeof(sum));
    level = 0.0;
    edge = 0.0;
    inner = 0.0;
    out->edges = 0;
    out->inners = 0;

    for (y = 0; y < HDRG_NOISE_TILE; y++) {
        for (x = 0; x < HDRG_NOISE_TILE; x++) {
            for (c = 0; c < 3; c++) {
                n[c] = h[c][y][x] - 2 * h[c][y + 1u][x] + h[c][y + 2u][x];
            }

            ny = k[0] * (float)n[0] + k[1] * (float)n[1] + k[2] * (float)n[2];
            sum[0] += (double)fabsf(ny);
            sum[1] += (double)fabsf(((float)n[2] - ny) / (2.0f * (1.0f - k[2])));
            sum[2] += (double)fabsf(((float)n[0] - ny) / (2.0f * (1.0f - k[0])));
            level += (double)luma[y + 1u][x + 1u];

            /*
             * steps to the right and down, across the block grid when they
             * leave a block; larger ones are edges wherever they are
             */
            step = fabsf(luma[y + 1u][x + 2u] - luma[y + 1u][x + 1u]);

            if (step <= HDRG_NOISE_STEP_MAX && (x0 + x) % HDRG_NOISE_GRID == HDRG_NOISE_GRID - 1u) {
                edge += (double)step;
                out->edges++;
            } else if (step <= HDRG_NOISE_STEP_MAX) {
                inner += (double)step;
                out->inners++;
            }

            step = fabsf(luma[y + 2u][x + 1u] - luma[y + 1u][x + 1u]);

            if (step <= HDRG_NOISE_STEP_MAX && (y0 + y) % HDRG_NOISE_GRID == HDRG_NOISE_GRID - 1u) {
                edge += (double)step;
                out->edges++;
            } else if (step <= HDRG_NOISE_STEP_MAX) {
                inner += (double)step;
                out->inners++;
            }
        }
    }

    for (c = 0; c < 3; c++) {
        out->sigma[c] =
            (float)(sum[c] / (double)(HDRG_NOISE_TILE * HDRG_NOISE_TILE)) * HDRG_NOISE_SCALE / top;
    }

    out->level = (float)(level / (double)(HDRG_NOISE_TILE * HDRG_NOISE_TILE));
    out->edge = (float)edge;
    out->inner = (float)inner;
    out->clipped = clipped;

    hdrg_noise_ring((const float (*)[HDRG_NOISE_SPAN])luma, out);
}

static void hdrg_noise_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_noise_job_t *job = (const hdrg_noise_job_t *)arg;
    const hdrg_noise_grid_t *grid = job->grid;
    uint32_t r, x0, y0;

    for (r = row_begin; r < row_end; r++) {
        x0 = (grid->first_x + (r % grid->nx) * grid->step) * HDRG_NOISE_TILE;
        y0 = (grid->first_y + (r / grid->nx) * grid->step) * HDRG_NOISE_TILE;

        hdrg_noise_tile(job->src, job->mosaic ? (r % grid->nx) * HDRG_NOISE_SPAN : x0 - 1u,
                        job->mosaic ? (r / grid->nx) * HDRG_NOISE_SPAN : y0 - 1u, x0, y0,
                        &job->tiles[r]);
    }
}

/*
 * the blockiness over the tiles at or below the median Y' level of the
 * unclipped ones into out, with the mean step the grid adds
 */
static void hdrg_noise_blockiness(const hdrg_noise_tile_t *tiles, uint32_t count, float median,
                                  hdrg_noise_t *out)
{
    uint32_t i, edges, inners;
    double edge, inner;
    float ratio;

    edges = 0;
    inners = 0;
    edge = 0.0;
    inner = 0.0;

    for (i = 0; i < count; i++) {
        if (tiles[i].clipped || tiles[i].sigma[0] > median) {
            continue;
        }

        edge += (double)tiles[i].edge;
        inner += (double)tiles[i].inner;
        edges += tiles[i].edges;
        inners += tiles[i].inners;
    }

    if (edges == 0 || inners == 0) {
        return;
    }

    edge /= (double)edges;
    inner /= (double)inners;

    if (edge < (double)HDRG_NOISE_STEP_MIN && inner < (double)HDRG_NOISE_STEP_MIN) {
        return;
    }

    ratio = (float)(edge / HDRG_MAX(inner, (double)HDRG_NOISE_STEP_MIN));
    out->blockiness = HDRG_MIN(ratio, HDRG_NOISE_BLOCK_MAX);
    out->block_step = (float)HDRG_MAX(edge - inner, 0.0);
}

/* the ringing over every tile: blocks with an edge against those without one */
static void hdrg_noise_ringing(const hdrg_noise_tile_t *tiles, uint32_t count, hdrg_noise_t *out)
{
    uint32_t i, rings, flats;
    double ring, flat;

    rings = 0;
    flats = 0;
    ring = 0.0;
    flat = 0.0;

    for (i = 0; i < count; i++) {
        ring += (double)tiles[i].ring;
        flat += (double)tiles[i].flat;
        rings += tiles[i].rings;
        flats += tiles[i].flats;
    }

    if (rings == 0 || flats == 0) {
        return;
    }

    ring /= (double)rings;
    flat /= (double)flats;

    if (ring < (double)HDRG_NOISE_STEP_MIN) {
        return;
    }

    out->ringing = (float)HDRG_MIN(ring / HDRG_MAX(flat, (double)HDRG_NOISE_STEP_MIN),
                                   (double)HDRG_NOISE_BLOCK_MAX);
    out->ring_level = (float)HDRG_MAX(ring - flat, 0.0);
}

/* the defaults: no data */
static void hdrg_noise_none(hdrg_noise_t *out)
{
    uint32_t b;

    out->sigma = -1.0f;
    out->sigma_cb = -1.0f;
    out->sigma_cr = -1.0f;
    out->blockiness = 1.0f;
    out->block_step = 0.0f;
    out->ringing = 1.0f;
    out->ring_level = 0.0f;
    out->tiles = 0;

    for (b = 0; b < HDRG_NOISE_BINS; b++) {
        out->sigma_y[b] = -1.0f;
    }
}

/* the grid's tiles of src (mosaic: src holds their windows) into out */
static hdrg_status_t hdrg_noise_measure(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                        const hdrg_source_t *src, const hdrg_noise_grid_t *grid,
                                        bool mosaic, hdrg_noise_t *out)
{
    hdrg_noise_job_t job;
    hdrg_noise_tile_t *tiles;
    uint32_t count, used, n, i, b;
    uint8_t c;
    float *values, median;

    count = grid->nx * grid->ny;

    tiles = (hdrg_noise_tile_t *)hdrg_mem_alloc(alloc, (size_t)count * sizeof(hdrg_noise_tile_t));
    values = (float *)hdrg_mem_alloc(alloc, (size_t)count * sizeof(float));
    if (tiles == NULL || values == NULL) {
        hdrg_mem_free(alloc, tiles);
        hdrg_mem_free(alloc, values);

        return HDRG_ERR_NOMEM;
    }

    job.src = src;
    job.grid = grid;
    job.tiles = tiles;
    job.mosaic = mosaic;

    hdrg_exec_rows(exec, count, hdrg_noise_rows, &job);
    hdrg_noise_ringing(tiles, count, out);

    for (c = 0; c < 3; c++) {
        used = 0;

        for (i = 0; i < count; i++) {
            if (!tiles[i].clipped) {
                values[used++] = tiles[i].sigma[c];
            }
        }

        if (used == 0) {
            break;
        }

        if (c == 0) {
            /* sorted Y' levels: the median splits the quieter half for the blockiness */
            out->sigma = hdrg_noise_level(values, used);
            median = values[(used - 1u) / 2u];
            hdrg_noise_blockiness(tiles, count, median, out);
            out->tiles = used;
        } else {
            *((c == 1) ? &out->sigma_cb : &out->sigma_cr) = hdrg_noise_level(values, used);
        }
    }

    for (b = 0; b < HDRG_NOISE_BINS && out->tiles > 0; b++) {
        n = 0;

        for (i = 0; i < count; i++) {
            if (!tiles[i].clipped && HDRG_MIN((uint32_t)(tiles[i].level * (float)HDRG_NOISE_BINS),
                                              HDRG_NOISE_BINS - 1u) == b) {
                values[n++] = tiles[i].sigma[0];
            }
        }

        if (n >= HDRG_NOISE_MIN_TILES) {
            out->sigma_y[b] = hdrg_noise_level(values, n);
        }
    }

    hdrg_mem_free(alloc, tiles);
    hdrg_mem_free(alloc, values);

    return HDRG_OK;
}

extern bool hdrg_noise_grid(uint32_t width, uint32_t height, hdrg_noise_grid_t *grid)
{
    uint32_t cx, cy;

    memset(grid, 0, sizeof(*grid));

    /* tile i spans [16 i, 16 i + 16); its window needs one pixel more on each side */
    cx = (width > 2u * HDRG_NOISE_TILE) ? (width - HDRG_NOISE_TILE - 1u) / HDRG_NOISE_TILE : 0u;
    cy = (height > 2u * HDRG_NOISE_TILE) ? (height - HDRG_NOISE_TILE - 1u) / HDRG_NOISE_TILE : 0u;

    if (cx == 0 || cy == 0) {
        return false;
    }

    grid->step = 1;
    while (((cx + grid->step - 1u) / grid->step) * ((cy + grid->step - 1u) / grid->step) >
           HDRG_NOISE_MAX_TILES) {
        grid->step++;
    }

    /* the grid centered on the candidate tiles 1..cx */
    grid->nx = (cx + grid->step - 1u) / grid->step;
    grid->ny = (cy + grid->step - 1u) / grid->step;
    grid->first_x = 1u + (cx - 1u - grid->step * (grid->nx - 1u)) / 2u;
    grid->first_y = 1u + (cy - 1u - grid->step * (grid->ny - 1u)) / 2u;

    return true;
}

extern hdrg_status_t hdrg_noise_estimate(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                         const hdrg_source_t *src, hdrg_noise_t *out)
{
    hdrg_noise_grid_t grid;

    hdrg_noise_none(out);

    if (!hdrg_noise_grid(src->width, src->height, &grid)) {
        return HDRG_OK;
    }

    return hdrg_noise_measure(alloc, exec, src, &grid, false, out);
}

extern hdrg_status_t hdrg_noise_estimate_mosaic(const hdrg_allocator_t *alloc,
                                                const hdrg_exec_t *exec,
                                                const hdrg_source_t *mosaic,
                                                const hdrg_noise_grid_t *grid, hdrg_noise_t *out)
{
    hdrg_noise_none(out);

    if (grid->nx == 0 || grid->ny == 0) {
        return HDRG_OK;
    }

    return hdrg_noise_measure(alloc, exec, mosaic, grid, true, out);
}

extern float hdrg_noise_at(const hdrg_noise_t *noise, float y)
{
    uint32_t b, lo, hi;
    float pos, t;

    pos = y * (float)HDRG_NOISE_BINS - 0.5f;
    lo = UINT32_MAX;
    hi = UINT32_MAX;

    for (b = 0; b < HDRG_NOISE_BINS; b++) {
        if (noise->sigma_y[b] < 0.0f) {
            continue;
        }

        if ((float)b <= pos) {
            lo = b;
        }

        if ((float)b >= pos && hi == UINT32_MAX) {
            hi = b;
        }
    }

    if (lo == UINT32_MAX && hi == UINT32_MAX) {
        return HDRG_MAX(noise->sigma, 0.0f);
    }

    if (lo == UINT32_MAX || hi == UINT32_MAX || lo == hi) {
        return noise->sigma_y[(lo == UINT32_MAX) ? hi : lo];
    }

    t = (pos - (float)lo) / (float)(hi - lo);

    return noise->sigma_y[lo] * (1.0f - t) + noise->sigma_y[hi] * t;
}
