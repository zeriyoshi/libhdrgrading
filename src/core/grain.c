/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "grain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/math.h"

#define HDRG_GRAIN_KEY        ((uint64_t)HDRG_GRAIN_KEY_HI << 32 | UINT64_C(0x5F505141))
#define HDRG_GRAIN_ONE        256.0f        /* the taps' Q8 */
#define HDRG_GRAIN_NOISE      147.80054127f /* the white noise's sigma: sqrt(4 (256^2 - 1) / 12) */
#define HDRG_GRAIN_ACROSS     4u            /* the horizontal pass's shift ... */
#define HDRG_GRAIN_DOWN       8u      /* ... and the vertical's: a unit 2^-12 of taps^2 noise */
#define HDRG_GRAIN_SCALE      4096.0f /* the scales' Q12 ... */
#define HDRG_GRAIN_MOST_SCALE 5000.0f /* ... at most (each product within 32 bits) */
#define HDRG_GRAIN_WHITE      0.02f   /* below this correlation the grain is white ... */
#define HDRG_GRAIN_MOST       0.95f   /* ... and it is held at this */
#define HDRG_GRAIN_CR_R       6040    /* BT.2020's Y'CbCr -> R'G'B' (Q12): R' = Y' + 1.4746 Cr */
#define HDRG_GRAIN_CB_G       674     /* G' = Y' - 0.16455 Cb - 0.57135 Cr */
#define HDRG_GRAIN_CR_G       2340
#define HDRG_GRAIN_CB_B       7706       /* B' = Y' + 1.8814 Cb */
#define HDRG_GRAIN_MIN_TILES  8u         /* a level's tiles at least */
#define HDRG_GRAIN_GAUSS      511.52514f /* AV1's Gaussian_Sequence: its sigma (of its 2048) */
#define HDRG_GRAIN_AV1_ROWS   73u        /* AV1's luma template: rows ... */
#define HDRG_GRAIN_AV1_COLS   82u        /* ... and columns */
#define HDRG_GRAIN_AV1_RUNS   4u         /* templates whose moments are summed */
#define HDRG_GRAIN_FILM_KEY   UINT64_C(0x46494C4D5F475241) /* their white noise's */

/* a tile of the noise grid: the removed signal's variances and the output's level */
typedef struct {
    float var[3]; /* Y', Cb, Cr */
    float d1, d2; /* Y''s steps to the next sample and the one after */
    float level;  /* the output's mean Y' */
    uint32_t index;
} hdrg_grain_tile_t;

/* a frame's windows into a mosaic (hdrg_grain_gather) */
typedef struct {
    const hdrg_noise_grid_t *grid;
    const uint16_t *rgb;
    size_t stride;
    uint16_t *mosaic;
} hdrg_grain_gather_t;

/* a template's moments over the part AV1's synthesis reads */
typedef struct {
    double squares, across, down; /* its samples', and with the next across and down */
    uint32_t n, n_across, n_down;
} hdrg_grain_moments_t;

/* the grid's windows before and after (mosaics: hdrg_grain_gather's) */
typedef struct {
    const hdrg_noise_grid_t *grid;
    hdrg_grain_tile_t *tiles;
    const uint16_t *before, *after;
    size_t stride;
    const float *coeffs;
} hdrg_grain_job_t;

/* round(v / 2^s) by the floor, for |v| below 2^61 (no shift of a negative value) */
static inline int64_t hdrg_grain_shift(int64_t v, uint8_t s)
{
    const uint64_t bias = UINT64_C(1) << 62;

    return (int64_t)(((uint64_t)(v + (INT64_C(1) << (s - 1u))) + bias) >> s) - (int64_t)(bias >> s);
}

/* the AV1 specification's Round2 of a signed value */
static inline int32_t hdrg_grain_round2(int32_t v, uint8_t s)
{
    return (s == 0) ? v : (int32_t)hdrg_grain_shift(v, s);
}

/* v / 2^s rounded down (the specification's >> of a signed value) */
static inline int32_t hdrg_grain_floor(int32_t v, uint8_t s)
{
    return (int32_t)hdrg_grain_shift((int64_t)v - (INT64_C(1) << (s - 1u)), s);
}

/* the integer nearest v */
static inline int32_t hdrg_grain_nearest(float v)
{
    return (v >= 0.0f) ? (int32_t)(v + 0.5f) : -(int32_t)(0.5f - v);
}

/* the source's Y' of codes (16-bit: wide) as an 8-bit level, in integers (the GPU's) */
static inline uint32_t hdrg_grain_level(const hdrg_grain_t *grain, const uint32_t codes[3],
                                        bool wide)
{
    const uint32_t luma = (grain->luma[0] * codes[0] + grain->luma[1] * codes[1] +
                           grain->luma[2] * codes[2] + 16384u) >>
                          15;

    return HDRG_MIN(wide ? (luma + 128u) / 257u : luma, 255u);
}

/* a white sample of mean 0: the sum of the hash's four bytes */
static inline int32_t hdrg_grain_white(uint32_t h)
{
    return (int32_t)((h & 0xffu) + ((h >> 8) & 0xffu) + ((h >> 16) & 0xffu) + (h >> 24)) - 510;
}

/* a tile's bin of the output's level */
static inline uint32_t hdrg_grain_bin(const hdrg_grain_tile_t *tile)
{
    return HDRG_MIN((uint32_t)(tile->level * (float)HDRG_NOISE_BINS), HDRG_NOISE_BINS - 1u);
}

/* a mosaic's row of windows in bytes */
static inline size_t hdrg_grain_mosaic_stride(const hdrg_noise_grid_t *grid)
{
    return (size_t)grid->nx * HDRG_NOISE_WINDOW * 3u * sizeof(uint16_t);
}

/* a 16-bit sample plus d, clamped */
static inline uint16_t hdrg_grain_add16(uint16_t v, int32_t d)
{
    const int32_t s = (int32_t)v + d;

    return (uint16_t)((s < 0) ? 0 : (s > 65535) ? 65535 : s);
}

/* an 8-bit sample plus d of 16-bit codes, clamped and rounded */
static inline uint8_t hdrg_grain_add8(uint8_t v, int32_t d)
{
    int32_t s;

    s = (int32_t)v * 257 + d;
    s = (s < 0) ? 0 : (s > 65535) ? 65535 : s;

    return (uint8_t)((s + 128) / 257);
}

/* Gaussian taps of sigma into h (unnormalized): their lag-1 correlation */
static float hdrg_grain_gaussian(float sigma, float h[HDRG_GRAIN_TAPS])
{
    uint32_t i;
    float lag, norm, d;

    for (i = 0; i < HDRG_GRAIN_TAPS; i++) {
        d = (float)i - (float)HDRG_GRAIN_RADIUS;
        h[i] = hdrg_math_expf(-d * d / (2.0f * sigma * sigma));
    }

    lag = 0.0f;
    norm = 0.0f;

    for (i = 0; i < HDRG_GRAIN_TAPS; i++) {
        norm += h[i] * h[i];
        lag += (i + 1u < HDRG_GRAIN_TAPS) ? h[i] * h[i + 1u] : 0.0f;
    }

    return lag / norm;
}

/* the plane's key of the frame's */
static uint64_t hdrg_grain_key(uint32_t key, uint32_t plane)
{
    return HDRG_GRAIN_KEY ^ (uint64_t)hdrg_hash_mix32(key * 3u + plane);
}

/* the plane's white noise of row r filtered across into out (width words) */
static void hdrg_grain_fill(const hdrg_grain_t *grain, uint32_t plane, uint32_t r, uint32_t width,
                            int32_t *scratch, int32_t *out)
{
    const uint32_t row = hdrg_hash_row(hdrg_grain_key(grain->key, plane), r);
    uint32_t i, x, t;
    int64_t sum;

    for (i = 0; i < width + 2u * HDRG_GRAIN_RADIUS; i++) {
        scratch[i] = hdrg_grain_white(hdrg_hash_pixel(row, i - HDRG_GRAIN_RADIUS));
    }

    for (x = 0; x < width; x++) {
        sum = 0;

        for (t = 0; t < HDRG_GRAIN_TAPS; t++) {
            sum += (int64_t)grain->taps[t] * scratch[x + t];
        }

        out[x] = (int32_t)hdrg_grain_shift(sum, HDRG_GRAIN_ACROSS);
    }
}

/* the variance of n values of sum and sum of squares */
static double hdrg_grain_variance(double sum, double squares, uint32_t n)
{
    const double mean = sum / (double)n;

    return HDRG_MAX(squares / (double)n - mean * mean, 0.0);
}

/* tiles by their removed Y' variance, then by their index: a total order */
static int hdrg_grain_compare(const void *a, const void *b)
{
    const hdrg_grain_tile_t *x = (const hdrg_grain_tile_t *)a, *y = (const hdrg_grain_tile_t *)b;

    if (x->var[0] != y->var[0]) {
        return (x->var[0] > y->var[0]) - (x->var[0] < y->var[0]);
    }

    return (x->index > y->index) - (x->index < y->index);
}

static int hdrg_grain_compare_values(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

/* the grid's rows of windows row_begin..row_end of a frame into the mosaic */
static void hdrg_grain_gather_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_grain_gather_t *job = (const hdrg_grain_gather_t *)arg;
    const hdrg_noise_grid_t *grid = job->grid;
    const uint16_t *from;
    const size_t row = hdrg_grain_mosaic_stride(grid) / sizeof(uint16_t);
    uint32_t r, x, y, x0, y0;

    for (r = row_begin; r < row_end; r++) {
        y0 = (grid->first_y + r * grid->step) * HDRG_NOISE_TILE - 1u;

        for (y = 0; y < HDRG_NOISE_WINDOW; y++) {
            for (x = 0; x < grid->nx; x++) {
                x0 = (grid->first_x + x * grid->step) * HDRG_NOISE_TILE - 1u;
                from = (const uint16_t *)(const void *)((const uint8_t *)job->rgb +
                                                        (size_t)(y0 + y) * job->stride) +
                       (size_t)x0 * 3u;
                memcpy(job->mosaic + ((size_t)r * HDRG_NOISE_WINDOW + y) * row +
                           (size_t)x * HDRG_NOISE_WINDOW * 3u,
                       from, HDRG_NOISE_WINDOW * 3u * sizeof(uint16_t));
            }
        }
    }
}

/* tile r of the grid: the removed Y', Cb and Cr's variances, Y''s steps' and the output's level */
static void hdrg_grain_tile_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_grain_job_t *job = (const hdrg_grain_job_t *)arg;
    const hdrg_noise_grid_t *grid = job->grid;
    const uint16_t *b, *a;
    const float *k = job->coeffs;
    hdrg_grain_tile_t *tile;
    uint32_t r, x0, y0, x, y, c;
    double sums[3][2], steps[4][2], level;
    float d[HDRG_NOISE_WINDOW][HDRG_NOISE_WINDOW], db, dr, step1, step2, top;

    top = 65535.0f;

    for (r = row_begin; r < row_end; r++) {
        x0 = (r % grid->nx) * HDRG_NOISE_WINDOW;
        y0 = (r / grid->nx) * HDRG_NOISE_WINDOW;
        tile = &job->tiles[r];
        memset(sums, 0, sizeof(sums));
        memset(steps, 0, sizeof(steps));
        level = 0.0;

        /* the removed Y' over the window, its Cb and Cr and the output's level over the tile */
        for (y = 0; y < HDRG_NOISE_WINDOW; y++) {
            b = (const uint16_t *)(const void *)((const uint8_t *)job->before +
                                                 (size_t)(y0 + y) * job->stride);
            a = (const uint16_t *)(const void *)((const uint8_t *)job->after +
                                                 (size_t)(y0 + y) * job->stride);

            for (x = 0; x < HDRG_NOISE_WINDOW; x++) {
                c = (x0 + x) * 3u;
                db = ((float)b[c + 2u] - (float)a[c + 2u]) / top;
                dr = ((float)b[c] - (float)a[c]) / top;
                d[y][x] =
                    k[0] * dr + k[1] * ((float)b[c + 1u] - (float)a[c + 1u]) / top + k[2] * db;

                if (x == 0u || y == 0u || x == HDRG_NOISE_WINDOW - 1u ||
                    y == HDRG_NOISE_WINDOW - 1u) {
                    continue;
                }

                db = (db - d[y][x]) / (2.0f * (1.0f - k[2]));
                dr = (dr - d[y][x]) / (2.0f * (1.0f - k[0]));
                sums[0][0] += (double)d[y][x];
                sums[0][1] += (double)d[y][x] * (double)d[y][x];
                sums[1][0] += (double)db;
                sums[1][1] += (double)db * (double)db;
                sums[2][0] += (double)dr;
                sums[2][1] += (double)dr * (double)dr;
                level += (double)(k[0] * (float)a[c] + k[1] * (float)a[c + 1u] +
                                  k[2] * (float)a[c + 2u]) /
                         (double)top;
            }
        }

        /* Y''s steps to the next sample and the one after, each way */
        for (y = 0; y < HDRG_NOISE_TILE; y++) {
            for (x = 0; x < HDRG_NOISE_TILE; x++) {
                step1 = d[y + 1u][x + 2u] - d[y + 1u][x + 1u];
                step2 = d[y + 1u][x + 2u] - d[y + 1u][x];
                steps[0][0] += (double)step1;
                steps[0][1] += (double)step1 * (double)step1;
                steps[1][0] += (double)step2;
                steps[1][1] += (double)step2 * (double)step2;
                step1 = d[y + 2u][x + 1u] - d[y + 1u][x + 1u];
                step2 = d[y + 2u][x + 1u] - d[y][x + 1u];
                steps[2][0] += (double)step1;
                steps[2][1] += (double)step1 * (double)step1;
                steps[3][0] += (double)step2;
                steps[3][1] += (double)step2 * (double)step2;
            }
        }

        for (c = 0; c < 3u; c++) {
            tile->var[c] = (float)hdrg_grain_variance(sums[c][0], sums[c][1],
                                                      HDRG_NOISE_TILE * HDRG_NOISE_TILE);
        }

        tile->d1 = (float)(0.5 * (hdrg_grain_variance(steps[0][0], steps[0][1],
                                                      HDRG_NOISE_TILE * HDRG_NOISE_TILE) +
                                  hdrg_grain_variance(steps[2][0], steps[2][1],
                                                      HDRG_NOISE_TILE * HDRG_NOISE_TILE)));
        tile->d2 = (float)(0.5 * (hdrg_grain_variance(steps[1][0], steps[1][1],
                                                      HDRG_NOISE_TILE * HDRG_NOISE_TILE) +
                                  hdrg_grain_variance(steps[3][0], steps[3][1],
                                                      HDRG_NOISE_TILE * HDRG_NOISE_TILE)));
        tile->level = (float)(level / (double)(HDRG_NOISE_TILE * HDRG_NOISE_TILE));
        tile->index = r;
    }
}

/*
 * the lag-1 correlation rho of noise whose second differences' variance is
 * ratio times its first differences': (1 + rho)(1 + rho^2) (a correlation
 * falling as a Gaussian: rho at lag 2 is rho^4), by bisection
 */
static float hdrg_grain_rho(double ratio)
{
    uint8_t i;
    double lo, hi, mid;

    if (ratio <= 1.0) {
        return 0.0f;
    }

    lo = 0.0;
    hi = 0.999;

    for (i = 0; i < 40u; i++) {
        mid = 0.5 * (lo + hi);

        if ((1.0 + mid) * (1.0 + mid * mid) < ratio) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    return (float)(0.5 * (lo + hi));
}

/* the standard deviation of the middle half of count sorted variances */
static float hdrg_grain_middle_values(const float *values, uint32_t count)
{
    uint32_t i, lo, hi;
    double sum;

    lo = count / 4u;
    hi = HDRG_MAX(count - count / 4u, lo + 1u);
    sum = 0.0;

    for (i = lo; i < hi; i++) {
        sum += (double)values[i];
    }

    return sqrtf((float)(sum / (double)(hi - lo)));
}

/*
 * AV1's scaling function of its count points (value, scaling pairs) by
 * 8-bit value (the specification's 7.18.3.5)
 */
static void hdrg_grain_av1_scaling(const uint8_t *points, uint8_t count,
                                   int32_t lut[HDRG_GRAIN_LEVELS])
{
    uint32_t i, x, dx;
    int32_t delta;

    for (x = 0; x < HDRG_GRAIN_LEVELS; x++) {
        lut[x] = (count == 0) ? 0 : (x < points[0]) ? points[1] : points[2u * (count - 1u) + 1u];
    }

    for (i = 0; i + 1u < count; i++) {
        dx = (uint32_t)points[2u * i + 2u] - points[2u * i];
        delta = ((int32_t)points[2u * i + 3u] - (int32_t)points[2u * i + 1u]) *
                (int32_t)((65536u + (dx >> 1)) / dx);

        for (x = 0; x < dx; x++) {
            lut[points[2u * i] + x] =
                points[2u * i + 1u] + (int32_t)hdrg_grain_shift((int64_t)x * delta, 16);
        }
    }
}

/*
 * One of AV1's grain templates (the specification's 7.18.3.3) of plane (0:
 * the luma; 1, 2: Cb, Cr over the luma's template luma) from run's white
 * noise of its Gaussian sequence's sigma into g, its moments over the part
 * the synthesis reads added to m
 */
static void hdrg_grain_av1_template(const hdrg_sd_film_grain_t *head,
                                    const hdrg_sd_film_grain_av1_t *p, uint32_t plane, uint32_t run,
                                    const int32_t *luma, int32_t *g, hdrg_grain_moments_t *m)
{
    const uint32_t sx = (plane == 0) ? 0u : head->subsampling_x,
                   sy = (plane == 0) ? 0u : head->subsampling_y,
                   rows = (sy != 0) ? 38u : HDRG_GRAIN_AV1_ROWS,
                   cols = (sx != 0) ? 44u : HDRG_GRAIN_AV1_COLS, y0 = (sy != 0) ? 6u : 9u,
                   y1 = (sy != 0) ? 38u : 73u, x0 = (sx != 0) ? 6u : 9u, x1 = (sx != 0) ? 38u : 73u;
    const uint8_t shift = (uint8_t)(12u - head->bit_depth + p->grain_scale_shift);
    const int32_t most = (int32_t)(128u << (head->bit_depth - 8u)), lag = p->ar_coeff_lag;
    const int8_t *coeffs = (plane == 0) ? p->ar_coeffs_y : p->ar_coeffs_uv[plane - 1u];
    uint32_t y, x, row, pos, i, j;
    int32_t dy, dx, sum, avg, v;
    double d;

    for (y = 0; y < rows; y++) {
        row = hdrg_hash_row(HDRG_GRAIN_FILM_KEY ^ ((uint64_t)plane << 8 | run), y);

        for (x = 0; x < cols; x++) {
            g[y * cols + x] = hdrg_grain_round2(
                hdrg_grain_nearest((float)hdrg_grain_white(hdrg_hash_pixel(row, x)) *
                                   (HDRG_GRAIN_GAUSS / HDRG_GRAIN_NOISE)),
                shift);
        }
    }

    for (y = 3; y < rows; y++) {
        for (x = 3; x + 3u < cols; x++) {
            sum = 0;
            pos = 0;

            for (dy = -lag; dy <= 0; dy++) {
                for (dx = -lag; dx <= lag && (dy < 0 || dx < 0); dx++) {
                    sum += g[(uint32_t)((int32_t)y + dy) * cols + (uint32_t)((int32_t)x + dx)] *
                           coeffs[pos++];
                }
            }

            /* chroma: the term of the luma's grain there */
            if (plane > 0 && p->num_y_points > 0) {
                avg = 0;

                for (j = 0; j <= sy; j++) {
                    for (i = 0; i <= sx; i++) {
                        avg += luma[(((y - 3u) << sy) + 3u + j) * HDRG_GRAIN_AV1_COLS +
                                    ((x - 3u) << sx) + 3u + i];
                    }
                }

                sum += hdrg_grain_round2(avg, (uint8_t)(sx + sy)) * coeffs[pos];
            }

            v = g[y * cols + x] + hdrg_grain_round2(sum, p->ar_coeff_shift);
            g[y * cols + x] = (v < -most) ? -most : (v > most - 1) ? most - 1 : v;
        }
    }

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            d = (double)g[y * cols + x];
            m->squares += d * d;
            m->n++;

            if (x + 1u < x1) {
                m->across += d * (double)g[y * cols + x + 1u];
                m->n_across++;
            }

            if (y + 1u < y1) {
                m->down += d * (double)g[(y + 1u) * cols + x];
                m->n_down++;
            }
        }
    }
}

/*
 * The measured grain by level into out: Y' between the centers of the bins
 * with tiles (held past the outer ones; sigma without any), Cb and Cr all
 * over
 */
static void hdrg_grain_levels(const float bins[HDRG_NOISE_BINS], float sigma, const float chroma[2],
                              hdrg_grain_stats_t *out)
{
    uint32_t level, b, lower, upper;
    float at, t;

    for (level = 0; level < HDRG_GRAIN_LEVELS; level++) {
        at = (float)level / 255.0f * (float)HDRG_NOISE_BINS - 0.5f;
        lower = HDRG_NOISE_BINS;
        upper = HDRG_NOISE_BINS;

        for (b = 0; b < HDRG_NOISE_BINS; b++) {
            if (bins[b] >= 0.0f && (float)b <= at) {
                lower = b;
            }

            if (bins[b] >= 0.0f && (float)b >= at && upper == HDRG_NOISE_BINS) {
                upper = b;
            }
        }

        lower = (lower == HDRG_NOISE_BINS) ? upper : lower;
        upper = (upper == HDRG_NOISE_BINS) ? lower : upper;
        t = (upper > lower) ? (at - (float)lower) / (float)(upper - lower) : 0.0f;
        out->sigma[0][level] =
            (lower == HDRG_NOISE_BINS) ? sigma : bins[lower] + t * (bins[upper] - bins[lower]);
        out->sigma[1][level] = HDRG_MAX(chroma[0], 0.0f);
        out->sigma[2][level] = HDRG_MAX(chroma[1], 0.0f);
    }

    out->valid = sigma > 0.0f;
}

extern size_t hdrg_grain_windows(const hdrg_noise_grid_t *grid)
{
    return (size_t)grid->nx * grid->ny * HDRG_NOISE_WINDOW * HDRG_NOISE_WINDOW * 3u;
}

extern void hdrg_grain_gather(const hdrg_exec_t *exec, const hdrg_noise_grid_t *grid,
                              const uint16_t *rgb, size_t stride, uint16_t *mosaic)
{
    hdrg_grain_gather_t job;

    job.grid = grid;
    job.rgb = rgb;
    job.stride = stride;
    job.mosaic = mosaic;

    hdrg_exec_rows(exec, grid->ny, hdrg_grain_gather_rows, &job);
}

extern hdrg_status_t hdrg_grain_measure(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                        const uint16_t *before, const uint16_t *after,
                                        const hdrg_noise_grid_t *grid, const float coeffs[3],
                                        hdrg_grain_stats_t *out)
{
    const uint32_t count = grid->nx * grid->ny;
    hdrg_grain_tile_t *tiles;
    hdrg_grain_job_t job;
    uint32_t i, lo, hi, n, b, at[HDRG_NOISE_BINS + 1u];
    double d1, d2;
    float *values, bins[HDRG_NOISE_BINS], sigma, chroma[2];

    memset(out, 0, sizeof(*out));
    out->valid = false;
    tiles = (hdrg_grain_tile_t *)hdrg_mem_alloc(alloc,
                                                (size_t)count * (sizeof(*tiles) + sizeof(*values)));
    if (tiles == NULL) {
        return HDRG_ERR_NOMEM;
    }

    job.grid = grid;
    job.tiles = tiles;
    job.before = before;
    job.after = after;
    job.stride = hdrg_grain_mosaic_stride(grid);
    job.coeffs = coeffs;

    hdrg_exec_rows(exec, count, hdrg_grain_tile_rows, &job);

    /* Cb and Cr of the tiles sorted by each, then Y' with its steps and levels */
    values = (float *)(void *)(tiles + count);

    for (i = 1; i < 3u; i++) {
        for (n = 0; n < count; n++) {
            values[n] = tiles[n].var[i];
        }

        qsort(values, count, sizeof(*values), hdrg_grain_compare_values);
        chroma[i - 1u] = hdrg_grain_middle_values(values, count);
    }

    qsort(tiles, count, sizeof(*tiles), hdrg_grain_compare);

    for (n = 0; n < count; n++) {
        values[n] = tiles[n].var[0];
    }

    sigma = hdrg_grain_middle_values(values, count);
    lo = count / 4u;
    hi = HDRG_MAX(count - count / 4u, lo + 1u);
    d1 = 0.0;
    d2 = 0.0;

    for (i = lo; i < hi; i++) {
        d1 += (double)tiles[i].d1;
        d2 += (double)tiles[i].d2;
    }

    out->rho = (d1 > 0.0) ? hdrg_grain_rho(d2 / d1) : 0.0f;

    /* by the output's level: the bin's tiles in the same order (each bin's run of values) */
    memset(at, 0, sizeof(at));

    for (i = 0; i < count; i++) {
        at[hdrg_grain_bin(&tiles[i]) + 1u]++;
    }

    for (b = 0; b < HDRG_NOISE_BINS; b++) {
        at[b + 1u] += at[b];
    }

    for (i = 0; i < count; i++) {
        values[at[hdrg_grain_bin(&tiles[i])]++] = tiles[i].var[0];
    }

    /* (bin b now ends at at[b]) */
    for (b = 0; b < HDRG_NOISE_BINS; b++) {
        n = (b > 0) ? at[b - 1u] : 0u;
        bins[b] = (at[b] - n >= HDRG_GRAIN_MIN_TILES)
                      ? hdrg_grain_middle_values(values + n, at[b] - n)
                      : -1.0f;
    }

    hdrg_mem_free(alloc, tiles);
    hdrg_grain_levels(bins, sigma, chroma, out);

    return HDRG_OK;
}

extern bool hdrg_grain_plan(const hdrg_grain_stats_t *stats, const hdrg_source_t *src,
                            float sdr_white_nits, float amount, uint32_t key, hdrg_grain_t *out)
{
    uint64_t squares;
    uint32_t i, level, code;
    int32_t prev, next;
    float h[HDRG_GRAIN_TAPS], pq[HDRG_GRAIN_LEVELS], rho, lo, hi, mid, norm, unit, slope, lin;

    if (!stats->valid || !(amount > 0.0f)) {
        return false;
    }

    /* the taps of the removed grain's correlation: a Gaussian's, by bisection on its sigma */
    rho = HDRG_MIN(HDRG_MAX(stats->rho, 0.0f), HDRG_GRAIN_MOST);
    lo = 0.05f;
    hi = 4.0f;

    for (i = 0; rho >= HDRG_GRAIN_WHITE && i < 30u; i++) {
        mid = 0.5f * (lo + hi);

        if (hdrg_grain_gaussian(mid, h) < rho) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    if (rho >= HDRG_GRAIN_WHITE) {
        (void)hdrg_grain_gaussian(hi, h);
    } else {
        memset(h, 0, sizeof(h));
        h[HDRG_GRAIN_RADIUS] = 1.0f;
    }

    norm = 0.0f;

    for (i = 0; i < HDRG_GRAIN_TAPS; i++) {
        norm += h[i] * h[i];
    }

    squares = 0;

    for (i = 0; i < HDRG_GRAIN_TAPS; i++) {
        out->taps[i] = (int32_t)(h[i] / sqrtf(norm) * HDRG_GRAIN_ONE + 0.5f);
        squares += (uint64_t)((int64_t)out->taps[i] * out->taps[i]);
    }

    /* the filtered noise's unit: the white noise's sigma through both passes and their shifts */
    unit = HDRG_GRAIN_NOISE * (float)squares /
           (float)(UINT32_C(1) << (HDRG_GRAIN_ACROSS + HDRG_GRAIN_DOWN));

    /* the PQ signal of each Y' level shown in SDR at its white */
    for (level = 0; level < HDRG_GRAIN_LEVELS; level++) {
        code = src->wide ? level * 257u : level;
        lin = src->coeffs[0] * src->lut[code] + src->coeffs[1] * src->lut[src->entries + code] +
              src->coeffs[2] * src->lut[2u * src->entries + code];
        pq[level] = hdrg_math_pq_oetf(HDRG_MAX(lin, 0.0f) * sdr_white_nits);
    }

    for (level = 0; level < HDRG_GRAIN_LEVELS; level++) {
        prev = (int32_t)((level > 0u) ? level - 1u : level);
        next = (int32_t)((level + 1u < HDRG_GRAIN_LEVELS) ? level + 1u : level);
        slope = (pq[next] - pq[prev]) / ((float)(next - prev) / 255.0f);

        for (i = 0; i < 3u; i++) {
            out->scale[i][level] = (int32_t)HDRG_MIN(
                stats->sigma[i][level] * slope * 65535.0f * amount / unit * HDRG_GRAIN_SCALE + 0.5f,
                HDRG_GRAIN_MOST_SCALE);
        }
    }

    for (i = 0; i < 3u; i++) {
        out->luma[i] = (uint32_t)(src->coeffs[i] * 32768.0f + 0.5f);
    }

    out->key = key;

    return true;
}

extern bool hdrg_grain_film_check(const void *data, size_t size)
{
    hdrg_sd_film_grain_t head;
    hdrg_sd_film_grain_av1_t p;
    uint32_t c, i;
    bool ok;

    if (data == NULL || size != HDRG_GRAIN_FILM_BYTES) {
        return false;
    }

    memcpy(&head, data, sizeof(head));
    memcpy(&p, (const uint8_t *)data + sizeof(head), sizeof(p));

    ok = head.kind == HDRG_FILM_GRAIN_AV1 && head.bit_depth >= 8u && head.bit_depth <= 12u &&
         head.full_range <= 1u && head.subsampling_x <= 1u &&
         head.subsampling_y <= head.subsampling_x && head.reserved == 0 && p.num_y_points <= 14u &&
         p.num_uv_points[0] <= 10u && p.num_uv_points[1] <= 10u &&
         p.chroma_scaling_from_luma <= 1u && p.scaling_shift >= 8u && p.scaling_shift <= 11u &&
         p.ar_coeff_lag <= 3u && p.ar_coeff_shift >= 6u && p.ar_coeff_shift <= 9u &&
         p.grain_scale_shift <= 3u && p.overlap <= 1u && p.reserved == 0;

    for (i = 1; ok && i < p.num_y_points; i++) {
        ok = p.y_points[i][0] > p.y_points[i - 1u][0];
    }

    for (c = 0; ok && c < 2u; c++) {
        ok = p.uv_mult[c] >= -128 && p.uv_mult[c] <= 127 && p.uv_mult_luma[c] >= -128 &&
             p.uv_mult_luma[c] <= 127 && p.uv_offset[c] >= -256 && p.uv_offset[c] <= 255;

        for (i = 1; ok && i < p.num_uv_points[c]; i++) {
            ok = p.uv_points[c][i][0] > p.uv_points[c][i - 1u][0];
        }
    }

    return ok;
}

extern void hdrg_grain_film(const void *data, hdrg_grain_stats_t *out)
{
    hdrg_sd_film_grain_t head;
    hdrg_sd_film_grain_av1_t p;
    hdrg_grain_moments_t m[3];
    uint32_t run, c, level, coded, merged;
    int32_t lut[3][HDRG_GRAIN_LEVELS], luma[HDRG_GRAIN_AV1_ROWS * HDRG_GRAIN_AV1_COLS],
        chroma[HDRG_GRAIN_AV1_ROWS * HDRG_GRAIN_AV1_COLS], v;
    bool present[3];
    float sigma[3], units[3], top, rho;

    memcpy(&head, data, sizeof(head));
    memcpy(&p, (const uint8_t *)data + sizeof(head), sizeof(p));
    memset(out, 0, sizeof(*out));
    memset(m, 0, sizeof(m));
    memset(luma, 0, sizeof(luma));

    present[0] = p.num_y_points > 0;
    present[1] = p.num_uv_points[0] > 0 || p.chroma_scaling_from_luma != 0;
    present[2] = p.num_uv_points[1] > 0 || p.chroma_scaling_from_luma != 0;

    /* the templates' grain: the luma's, then Cb's and Cr's over it */
    for (run = 0; run < HDRG_GRAIN_AV1_RUNS; run++) {
        if (present[0]) {
            hdrg_grain_av1_template(&head, &p, 0, run, NULL, luma, &m[0]);
        }

        for (c = 1; c < 3u; c++) {
            if (present[c]) {
                hdrg_grain_av1_template(&head, &p, c, run, luma, chroma, &m[c]);
            }
        }
    }

    for (c = 0; c < 3u; c++) {
        sigma[c] = (m[c].n > 0) ? sqrtf((float)(m[c].squares / (double)m[c].n)) : 0.0f;
    }

    hdrg_grain_av1_scaling(&p.y_points[0][0], p.num_y_points, lut[0]);

    for (c = 1; c < 3u; c++) {
        if (p.chroma_scaling_from_luma != 0) {
            memcpy(lut[c], lut[0], sizeof(lut[0]));
        } else {
            hdrg_grain_av1_scaling(&p.uv_points[c - 1u][0][0], p.num_uv_points[c - 1u], lut[c]);
        }
    }

    /* the codes' full scale: Y' and Cb, Cr of the coded range */
    top = (float)((1u << head.bit_depth) - 1u);
    units[0] = (head.full_range != 0) ? top : (float)(219u << (head.bit_depth - 8u));
    units[1] = (head.full_range != 0) ? top : (float)(224u << (head.bit_depth - 8u));
    units[2] = units[1];

    /* by the source's Y' level coded: Cb and Cr at the neutral chroma */
    for (level = 0; level < HDRG_GRAIN_LEVELS; level++) {
        coded = (head.full_range != 0) ? level : 16u + (level * 219u + 127u) / 255u;

        for (c = 0; c < 3u; c++) {
            v = hdrg_grain_floor((int32_t)coded * p.uv_mult_luma[(c > 0) ? c - 1u : 0u] +
                                     128 * p.uv_mult[(c > 0) ? c - 1u : 0u],
                                 6) +
                p.uv_offset[(c > 0) ? c - 1u : 0u];
            merged = (c == 0 || p.chroma_scaling_from_luma != 0) ? coded
                                                                 : (uint32_t)((v < 0)     ? 0
                                                                              : (v > 255) ? 255
                                                                                          : v);
            out->sigma[c][level] =
                (float)lut[c][merged] * sigma[c] / (float)(1u << p.scaling_shift) / units[c];
            out->valid = out->valid || out->sigma[c][level] > 0.0f;
        }
    }

    /* the correlation: the luma's (else Cb's) across and down */
    c = present[0] ? 0u : 1u;
    rho = (m[c].n > 0 && m[c].squares > 0.0)
              ? (float)(0.5 *
                        (m[c].across / (double)m[c].n_across + m[c].down / (double)m[c].n_down) /
                        (m[c].squares / (double)m[c].n))
              : 0.0f;
    out->rho = HDRG_MAX(rho, 0.0f);
}

extern uint32_t hdrg_grain_key_of(uint32_t index, uint32_t phase)
{
    return hdrg_hash_mix32(index ^ hdrg_hash_mix32(phase + 1u));
}

extern uint32_t hdrg_grain_plane_key(const hdrg_grain_t *grain, uint32_t plane)
{
    return (uint32_t)hdrg_grain_key(grain->key, plane);
}

extern void hdrg_grain_words(const hdrg_grain_t *grain, uint32_t *words)
{
    uint32_t i, p;

    for (p = 0; p < 3u; p++) {
        words[p] = hdrg_grain_plane_key(grain, p);
        words[3u + p] = grain->luma[p];
    }

    for (i = 0; i < HDRG_GRAIN_TAPS; i++) {
        words[6u + i] = (uint32_t)grain->taps[i];
    }

    for (p = 0; p < 3u; p++) {
        for (i = 0; i < HDRG_GRAIN_LEVELS; i++) {
            words[6u + HDRG_GRAIN_TAPS + p * HDRG_GRAIN_LEVELS + i] = (uint32_t)grain->scale[p][i];
        }
    }
}

extern void hdrg_grain_row(const hdrg_grain_t *grain, const hdrg_source_t *src,
                           const uint8_t *src_row, uint32_t y, bool first, int32_t *rows,
                           int32_t *scratch, uint8_t *dst, uint8_t sample_bytes)
{
    const uint32_t width = src->width;
    uint32_t p, t, x, codes[3], level;
    uint16_t *dst16;
    uint8_t c;
    int64_t v;
    int32_t g[3], d[3];

    /* each plane's rows of the taps around y, filtered across: all of them first, else the new */
    for (p = 0; p < 3u; p++) {
        for (t = first ? 0u : HDRG_GRAIN_TAPS - 1u; t < HDRG_GRAIN_TAPS; t++) {
            hdrg_grain_fill(grain, p, y + t - HDRG_GRAIN_RADIUS, width, scratch,
                            rows +
                                ((size_t)p * HDRG_GRAIN_TAPS + (y + t) % HDRG_GRAIN_TAPS) * width);
        }
    }

    dst16 = (uint16_t *)(void *)dst;

    for (x = 0; x < width; x++) {
        hdrg_source_codes(src, src_row, x, codes);
        level = hdrg_grain_level(grain, codes, src->wide);

        for (p = 0; p < 3u; p++) {
            v = 0;

            for (t = 0; t < HDRG_GRAIN_TAPS; t++) {
                v += (int64_t)grain->taps[t] *
                     rows[((size_t)p * HDRG_GRAIN_TAPS + (y + t) % HDRG_GRAIN_TAPS) * width + x];
            }

            v = hdrg_grain_shift(v, HDRG_GRAIN_DOWN);
            g[p] = (int32_t)hdrg_grain_shift(v * grain->scale[p][level], 12u);
        }

        d[0] = g[0] + (int32_t)hdrg_grain_shift((int64_t)HDRG_GRAIN_CR_R * g[2], 12u);
        d[1] = g[0] - (int32_t)hdrg_grain_shift(
                          (int64_t)HDRG_GRAIN_CB_G * g[1] + (int64_t)HDRG_GRAIN_CR_G * g[2], 12u);
        d[2] = g[0] + (int32_t)hdrg_grain_shift((int64_t)HDRG_GRAIN_CB_B * g[1], 12u);

        for (c = 0; c < 3u; c++) {
            if (sample_bytes == 2u) {
                dst16[(size_t)x * 3u + c] = hdrg_grain_add16(dst16[(size_t)x * 3u + c], d[c]);
            } else {
                dst[(size_t)x * 3u + c] = hdrg_grain_add8(dst[(size_t)x * 3u + c], d[c]);
            }
        }
    }
}
