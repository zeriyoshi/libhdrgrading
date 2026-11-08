/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "analyze.h"

#include <math.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "exec.h"
#include "predict.h"

#define HDRG_HIST_BINS   HDRG_ANALYZE_BINS
#define HDRG_CLIP_LINEAR 0.991050f /* srgb_eotf(254/255) */
#define HDRG_CELL_SUMS                                                                             \
    5u /* per cell in a row of cells: La sum, La max, channel max, Y sum, the                      \
         chroma at the La max */

/*
 * Lights. A region grows over cells whose brightest pixel
 * reaches GROW (a light's core and its brightest glow, not a bright sky or
 * a sunlit wall next to it) and needs a cell whose brightest channel
 * reaches SEED (Didyk et al. 2008's clipped regions: seeds above code 230).
 * Its surround is the cells within RINGS of it (a chamfer distance of 3
 * across and 4 diagonally, rounded to cells: rings close to circles). A glow
 * (a bloom, a specular's soft edge) falls off around its light in every
 * direction alike: each cell of the first ring is compared with the cell
 * REACH cells farther out, away from the region cells next to it (along a
 * thin light's normal, from a compact one's center; not where it lands on
 * a bright region, another object), in
 * relative luminance Y (La follows a saturated channel: in a blue sky it
 * stays flat where a glow's luminance falls). The mean of these radial
 * drops over the step up to the region ramps the boost from GLOW_LO to
 * GLOW_HI (a background's even gradient cancels around the ring), the mean
 * itself from STEP to twice that; their spread over their mean, from
 * SPREAD - SOFT to SPREAD + SOFT, takes it back (a ring over several
 * surfaces, a window's glass and its wall, drops unevenly; a region of a
 * few cells, whose ring the cell grid makes uneven itself, is allowed up to
 * SPREAD_SMALL). Along each ray the drop is to fall all the way: the drops'
 * share of the rays' travel (the changes of Y from cell to cell out to the
 * drop's end) ramps it in from FALL_LO to FALL_HI (a glow falls
 * monotonically; foam on water, gravel or foliage around a bright patch
 * rises and falls, and its judgment would follow the grid's phase from
 * frame to frame). Nor do the drops lean to one side: their covariance with
 * their rays' directions over the sum of their sizes (0 for drops even
 * around the light, whatever directions its rays take: a light cut by the
 * frame's edge too) takes it back from LEAN_LO to LEAN_HI (a pale fill
 * bordering a darker one on one side, a glint on water by a dark bank). Soft ramps, so a
 * light judged near a threshold does not flip from frame to frame. A
 * clipped region without a glow (a white frame, plaster, text, a UI panel)
 * stays diffuse: clipping alone does not tell a light from a white surface.
 * Under a session's fade the thresholds follow the dimming.
 *
 * Colored lights (hdrg_params_t.colored_lights): a saturated light's La stays
 * at 0.7 of its channel (a red neon tube, an LED, an orange sun), below GROW.
 * Cells whose brightest channel reaches GROW while their La does not grow
 * regions of their own, judged as above; those touching a region of GROW
 * are its colored rim and are left out, and so are those without a clipped
 * channel (code 254): a cel-shaded fill's lightest spot, a gold trim or a
 * hair's tint peaks below it. A light lights its surround: where
 * the first ring's drop exceeds the step down from the region itself
 * (RING_LO to RING_HI: a sky clipped in blue over a hazy horizon, brighter
 * in Y than the sky), the region is not a light; nor is one covering
 * SHARE_LO to SHARE_HI of the frame and more (a sky whose haze still clips
 * in blue, a wall: an orange sun with its glow covers 1%).
 */
#define HDRG_LIGHT_GROW        0.85f /* La of a cell's brightest pixel */
#define HDRG_LIGHT_SEED        0.79f /* a cell's brightest channel (code 230) */
#define HDRG_LIGHT_RINGS       6u    /* cells of 4 pixels (24 pixels; finer cells: more) */
#define HDRG_GLOW_REACH        5.0f  /* cells of 4 pixels beyond the first ring a drop reads */
#define HDRG_GLOW_RAYS         4u    /* first-ring cells a judgment needs */
#define HDRG_GLOW_LO           0.03f
#define HDRG_GLOW_HI           0.12f
#define HDRG_GLOW_STEP         0.02f /* Y the mean drop ramps in from */
#define HDRG_GLOW_SPREAD       0.6f  /* the drops' standard deviation over their mean, ... */
#define HDRG_GLOW_SPREAD_SMALL 0.9f  /* ... of a region of SMALL_LO cells or fewer, to SMALL_HI */
#define HDRG_GLOW_SOFT         0.15f
#define HDRG_GLOW_SMALL_LO     4.0f /* cells of 4 x 4 pixels */
#define HDRG_GLOW_SMALL_HI     16.0f
#define HDRG_GLOW_FALL_LO      0.5f /* the drops over the rays' travel, cell to cell (a glow: 1) */
#define HDRG_GLOW_FALL_HI      0.7f
#define HDRG_GLOW_LEAN_LO      0.5f /* the drops' lean to one side (a glow: 0) */
#define HDRG_GLOW_LEAN_HI      0.6f
#define HDRG_COLORED_RING_LO   0.8f /* a colored region's first-ring drop over its step down */
#define HDRG_COLORED_RING_HI   1.0f
#define HDRG_COLORED_SHARE_LO  0.05f /* a colored region's share of the frame's cells */
#define HDRG_COLORED_SHARE_HI  0.1f
#define HDRG_COLORED_KEEP      0.05f /* a colored region judged below this is no light */
#define HDRG_COLORED_CHROMA_LO 0.7f  /* its cells' chroma at their brightest pixel, mean */
#define HDRG_COLORED_CHROMA_HI 0.85f
#define HDRG_COLORED_OUTER_LO  0.2f /* the Y its drops end at, mean (a light in the dark) */
#define HDRG_COLORED_OUTER_HI  0.3f
#define HDRG_LIGHT_FLOOR       0.0001f /* the ramp's ends in La: log2 floors here */
#define HDRG_FINE_SMALL        16u     /* QUALITY: fine cells (2 x 2) of the small lights it adds */
#define HDRG_FINE_ZONE         3u   /* ... the fine rings their boost reaches; their Y over ... */
#define HDRG_FINE_CONTRAST     1.7f /* ... the brightest cell around (24 pixels) at least this */
#define HDRG_DIST_NONE         UINT16_MAX
#define HDRG_DIST_FAR          1000u /* chamfer distances stop growing here, far past the rings */

/*
 * Pass A over the source: the histogram (per worker, summed
 * afterwards: integer counts do not depend on the split), per-cell mean La,
 * brightest La, brightest channel and mean Y. A job row range covers whole rows of
 * cells, so every cell is written by one job.
 */
typedef struct {
    const hdrg_source_t *src;
    uint64_t *counts; /* per worker: HDRG_HIST_BINS bins */
    float *scratch;   /* per worker: the La, channel max and Y rows, then the cell sums */
    float *qluma;     /* per cell: mean La */
    float *qmax;      /* per cell: brightest La */
    float *qpeak;     /* per cell: brightest channel */
    float *qchroma; /* per cell: the chroma (brightest - darkest channel) of its brightest pixel */
    float *qy;      /* per cell: mean relative luminance Y */
    size_t scratch_stride;
    uint32_t qw;
    uint32_t x0, x1, y0, y1; /* the statistics' rectangle (inside the black bars) */
    uint8_t shift;           /* log2 of the cells' side */
} hdrg_stats_job_t;

/* one judgment of the lights over a grid of cells */
typedef struct {
    float scale; /* a fade's dimming: the levels follow it */
    uint32_t qw, qh;
    uint32_t small; /* the largest region judged, cells (0: any) */
    uint32_t zone;  /* the rings a light's boost reaches */
    uint8_t shift;  /* log2 of the cells' side */
    bool colored;   /* judge the colored lights too (hdrg_colored_regions) */
} hdrg_lights_args_t;

/* per component of the bright cells: what judges it */
typedef struct {
    double luma;           /* sum of the mean Y of its cells */
    double drop, drop2;    /* sums of the radial drops of Y of its first ring and their squares */
    double travel;         /* sum of the rays' travel: |change of Y| from cell to cell */
    double lean_x, lean_y; /* sums of the drops along their rays' directions ... */
    double dir_x, dir_y;   /* ... of the directions ... */
    double size;           /* ... and of the drops' sizes */
    double outer, outer_y; /* sums of the La and the Y the drops end at */
    double chroma;         /* sum of its cells' chroma at their brightest pixel */
    float top;             /* brightest La */
    float peak;            /* brightest channel */
    float around;          /* the brightest Y of the cells around (its rings past the first) */
    uint32_t cells, rays;
    uint32_t x0, y0, x1, y1; /* its cells' bounding box */
    uint32_t index;          /* judged: its place in the list of lights */
    bool seeded;             /* holds a seed cell */
    bool colored;            /* grown over the saturated cells (hdrg_colored_regions) */
    bool judged;
    float boost, lo, hi; /* the judgment: the boost and its ramp (log2 La) */
} hdrg_region_t;

/* the judged lights of a grid (hdrg_analysis_t.light_of, lights, light_count) */
typedef struct {
    uint32_t *of;
    hdrg_light_t *lights;
    uint32_t count;
} hdrg_light_list_t;

/* a shorter chamfer distance to cell i through its neighbor n (step: 3 across, 4 diagonally) */
static inline void hdrg_chamfer(uint16_t *dist, uint32_t *owner, size_t i, size_t n, uint16_t step)
{
    if (dist[n] != HDRG_DIST_NONE && (uint32_t)dist[n] + step < dist[i]) {
        dist[i] = (uint16_t)HDRG_MIN((uint32_t)dist[n] + step, HDRG_DIST_FAR);
        owner[i] = owner[n];
    }
}

static inline float hdrg_smoothstep(float lo, float hi, float v)
{
    float t;

    t = HDRG_CLAMP((v - lo) / (hi - lo), 0.0f, 1.0f);

    return t * t * (3.0f - 2.0f * t);
}

/* every pixel of row y in [x0, x1) is black-bar dark */
static bool hdrg_bar_row(const hdrg_source_t *src, uint32_t y, uint32_t x0, uint32_t x1)
{
    const uint8_t *row = hdrg_source_row(src, y);
    uint32_t x;
    float rgb[3];

    for (x = x0; x < x1; x++) {
        hdrg_source_rgb(src, row, x, rgb);

        if (HDRG_MAX(rgb[0], HDRG_MAX(rgb[1], rgb[2])) >= HDRG_BAR_LIGHT) {
            return false;
        }
    }

    return true;
}

/* every pixel of column x in rows [y0, y1) is black-bar dark */
static bool hdrg_bar_column(const hdrg_source_t *src, uint32_t x, uint32_t y0, uint32_t y1)
{
    uint32_t y;
    float rgb[3];

    for (y = y0; y < y1; y++) {
        hdrg_source_rgb(src, hdrg_source_row(src, y), x, rgb);

        if (HDRG_MAX(rgb[0], HDRG_MAX(rgb[1], rgb[2])) >= HDRG_BAR_LIGHT) {
            return false;
        }
    }

    return true;
}

/* the black bars at the edges (top, bottom, left, right); none when under a quarter remains */
static void hdrg_bars(const hdrg_source_t *src, uint32_t bars[4])
{
    const uint32_t width = src->width, height = src->height;

    memset(bars, 0, 4u * sizeof(uint32_t));

    while (bars[0] < height && hdrg_bar_row(src, bars[0], 0, width)) {
        bars[0]++;
    }

    while (bars[1] < height - bars[0] && hdrg_bar_row(src, height - 1u - bars[1], 0, width)) {
        bars[1]++;
    }

    while (bars[2] < width && hdrg_bar_column(src, bars[2], bars[0], height - bars[1])) {
        bars[2]++;
    }

    while (bars[3] < width - bars[2] &&
           hdrg_bar_column(src, width - 1u - bars[3], bars[0], height - bars[1])) {
        bars[3]++;
    }

    if ((uint64_t)(width - bars[2] - bars[3]) * (height - bars[0] - bars[1]) * 4u <
        (uint64_t)width * height) {
        memset(bars, 0, 4u * sizeof(uint32_t));
    }
}

/* La, the brightest channel and Y of each pixel of row y */
static void hdrg_la_row(const hdrg_source_t *src, uint32_t y, float *la, float *peak, float *luma,
                        float *chroma)
{
    const uint8_t *row = hdrg_source_row(src, y);
    uint32_t x;
    float rgb[3];

    for (x = 0; x < src->width; x++) {
        hdrg_source_rgb(src, row, x, rgb);
        luma[x] = hdrg_rgb_y(src->coeffs, rgb);
        la[x] = hdrg_rgb_la(luma[x], rgb);
        peak[x] = HDRG_MAX(rgb[0], HDRG_MAX(rgb[1], rgb[2]));
        chroma[x] = peak[x] - HDRG_MIN(rgb[0], HDRG_MIN(rgb[1], rgb[2]));
    }
}

static void hdrg_stats_row(const hdrg_stats_job_t *job, uint32_t y, const float *la,
                           const float *peak, const float *luma, const float *chroma,
                           uint64_t *counts, float *qsum)
{
    const bool counted = y >= job->y0 && y < job->y1;
    uint32_t x, bin;
    float *cell;

    for (x = 0; x < job->src->width; x++) {
        if (counted && x >= job->x0 && x < job->x1) {
            /* NaN (a broken curve) counts as black: never an index out of the histogram */
            bin = (uint32_t)(((la[x] > 0.0f) ? HDRG_MIN(la[x], 1.0f) : 0.0f) *
                             (float)(HDRG_HIST_BINS - 1));
            counts[bin]++;
        }

        cell = qsum + (size_t)(x >> job->shift) * HDRG_CELL_SUMS;
        cell[0] += la[x];

        if (la[x] > cell[1]) {
            cell[4] = chroma[x];
        }

        cell[1] = HDRG_MAX(cell[1], la[x]);
        cell[2] = HDRG_MAX(cell[2], peak[x]);
        cell[3] += luma[x];
    }
}

/* the cells of one finished row of cells; rows = source rows it covers */
static void hdrg_stats_cells(const hdrg_stats_job_t *job, uint32_t qy, uint32_t rows,
                             const float *qsum)
{
    const uint32_t width = job->src->width;
    uint32_t qx, cols;
    size_t idx;

    for (qx = 0; qx < job->qw; qx++) {
        cols = HDRG_MIN(width - (qx << job->shift), 1u << job->shift);
        idx = (size_t)qy * job->qw + qx;
        job->qluma[idx] = qsum[(size_t)qx * HDRG_CELL_SUMS] / (float)(rows * cols);
        job->qmax[idx] = qsum[(size_t)qx * HDRG_CELL_SUMS + 1u];
        job->qpeak[idx] = qsum[(size_t)qx * HDRG_CELL_SUMS + 2u];
        job->qy[idx] = qsum[(size_t)qx * HDRG_CELL_SUMS + 3u] / (float)(rows * cols);
        job->qchroma[idx] = qsum[(size_t)qx * HDRG_CELL_SUMS + 4u];
    }
}

static void hdrg_stats_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_stats_job_t *job = (const hdrg_stats_job_t *)arg;
    const uint32_t width = job->src->width;
    uint64_t *counts;
    uint32_t y;
    float *la, *peak, *luma, *chroma, *qsum;

    counts = job->counts + (size_t)worker * HDRG_HIST_BINS;
    la = job->scratch + (size_t)worker * job->scratch_stride;
    peak = la + width;
    luma = peak + width;
    chroma = luma + width;
    qsum = chroma + width;

    for (y = row_begin; y < row_end; y++) {
        if ((y & ((1u << job->shift) - 1u)) == 0) {
            memset(qsum, 0, (size_t)job->qw * HDRG_CELL_SUMS * sizeof(float));
        }

        hdrg_la_row(job->src, y, la, peak, luma, chroma);
        hdrg_stats_row(job, y, la, peak, luma, chroma, counts, qsum);

        if ((y & ((1u << job->shift) - 1u)) == (1u << job->shift) - 1u || y + 1 == row_end) {
            hdrg_stats_cells(job, y >> job->shift, y - ((y >> job->shift) << job->shift) + 1, qsum);
        }
    }
}

static uint32_t hdrg_uf_find(uint32_t *parent, uint32_t v)
{
    uint32_t root, next;

    root = v;
    while (parent[root] != root) {
        root = parent[root];
    }

    while (parent[v] != root) {
        next = parent[v];
        parent[v] = root;
        v = next;
    }

    return root;
}

static void hdrg_uf_union(uint32_t *parent, uint32_t a, uint32_t b)
{
    uint32_t ra, rb;

    ra = hdrg_uf_find(parent, a);
    rb = hdrg_uf_find(parent, b);
    if (ra < rb) {
        parent[rb] = ra;
    } else if (rb < ra) {
        parent[ra] = rb;
    }
}

/*
 * each region's judgment from its outward drops; scale: a fade's dimming, the
 * levels follow it; area: cells a 4 x 4 pixel cell spans; frame: the cells of
 * the frame
 */
static void hdrg_judge(float scale, float area, float frame, hdrg_region_t *region)
{
    double average, lean_x, lean_y;
    float r, b, mean, sd, spread, fall, ratio, lean, glow;

    region->boost = 0.0f;

    if (!region->seeded || region->rays < HDRG_GLOW_RAYS) {
        return;
    }

    r = (float)(region->luma / (double)region->cells);
    b = (float)(region->outer_y / (double)region->rays);
    mean = (float)(region->drop / (double)region->rays);
    sd = sqrtf(HDRG_MAX((float)(region->drop2 / (double)region->rays) - mean * mean, 0.0f));
    spread = HDRG_GLOW_SPREAD_SMALL +
             (HDRG_GLOW_SPREAD - HDRG_GLOW_SPREAD_SMALL) *
                 HDRG_CLAMP(((float)region->cells - HDRG_GLOW_SMALL_LO * area) /
                                ((HDRG_GLOW_SMALL_HI - HDRG_GLOW_SMALL_LO) * area),
                            0.0f, 1.0f);
    fall = (float)(region->drop / HDRG_MAX(region->travel, 1e-9));
    average = region->drop / (double)region->rays;
    lean_x = region->lean_x - average * region->dir_x;
    lean_y = region->lean_y - average * region->dir_y;
    lean = (float)(sqrt(lean_x * lean_x + lean_y * lean_y) / HDRG_MAX(region->size, 1e-9));
    ratio = mean / HDRG_MAX(r - b, HDRG_GLOW_STEP * scale);
    glow = 0.0f;

    if (mean > HDRG_GLOW_STEP * scale) {
        glow =
            hdrg_smoothstep(HDRG_GLOW_LO, HDRG_GLOW_HI, ratio) *
            hdrg_smoothstep(HDRG_GLOW_STEP * scale, 2.0f * HDRG_GLOW_STEP * scale, mean) *
            (1.0f - hdrg_smoothstep(spread - HDRG_GLOW_SOFT, spread + HDRG_GLOW_SOFT, sd / mean)) *
            hdrg_smoothstep(HDRG_GLOW_FALL_LO, HDRG_GLOW_FALL_HI, fall) *
            (1.0f - hdrg_smoothstep(HDRG_GLOW_LEAN_LO, HDRG_GLOW_LEAN_HI, lean));
    }

    if (region->colored) {
        glow *= (1.0f - hdrg_smoothstep(HDRG_COLORED_RING_LO, HDRG_COLORED_RING_HI, ratio)) *
                (1.0f - hdrg_smoothstep(HDRG_COLORED_SHARE_LO, HDRG_COLORED_SHARE_HI,
                                        (float)region->cells / frame)) *
                hdrg_smoothstep(HDRG_COLORED_CHROMA_LO * scale, HDRG_COLORED_CHROMA_HI * scale,
                                (float)(region->chroma / (double)region->cells)) *
                (1.0f -
                 hdrg_smoothstep(HDRG_COLORED_OUTER_LO * scale, HDRG_COLORED_OUTER_HI * scale, b));
    } else {
        glow *= hdrg_smoothstep(HDRG_LIGHT_GROW * scale, HDRG_CLIP_LINEAR * scale, region->peak);
    }

    region->boost = glow;
    region->lo =
        hdrg_math_log2f(HDRG_MAX((float)(region->outer / (double)region->rays), HDRG_LIGHT_FLOOR));
    region->hi = hdrg_math_log2f(HDRG_MAX(region->top, HDRG_LIGHT_FLOOR));
}

/*
 * The colored lights' regions: components of the cells whose brightest
 * channel reaches GROW while their La does not, holding a clipped one and
 * not touching a region of labels (count of them), labeled after them;
 * the others do not take a white region's rings. scratch: a plane of labels.
 */
static hdrg_status_t hdrg_colored_regions(const hdrg_allocator_t *alloc,
                                          const hdrg_lights_args_t *args, const float *qmax,
                                          const float *qpeak, uint8_t *mask, uint32_t *labels,
                                          uint32_t *scratch, uint32_t *count)
{
    const uint32_t qw = args->qw, qh = args->qh;
    const size_t total = (size_t)qw * qh;
    const float grow = HDRG_LIGHT_GROW * args->scale, clip = HDRG_CLIP_LINEAR * args->scale;
    hdrg_status_t status;
    uint32_t *keep, found, next, label, x, y, nx, ny;
    size_t i;

    for (i = 0; i < total; i++) {
        mask[i] = (qpeak[i] >= grow && qmax[i] < grow) ? 1u : 0u;
    }

    status = hdrg_label_components(alloc, mask, qw, qh, scratch, &found);
    if (status != HDRG_OK || found == 0u) {
        return status;
    }

    /* per component: 1 holds a clipped cell, 2 touches a region of labels */
    keep = (uint32_t *)hdrg_mem_calloc(alloc, (size_t)found + 1u, sizeof(uint32_t));
    if (keep == NULL) {
        return HDRG_ERR_NOMEM;
    }

    for (y = 0; y < qh; y++) {
        for (x = 0; x < qw; x++) {
            i = (size_t)y * qw + x;

            if (scratch[i] == 0u) {
                continue;
            }

            keep[scratch[i]] |= (qpeak[i] >= clip) ? 1u : 0u;

            for (ny = (y > 0u) ? y - 1u : 0u; ny <= HDRG_MIN(y + 1u, qh - 1u); ny++) {
                for (nx = (x > 0u) ? x - 1u : 0u; nx <= HDRG_MIN(x + 1u, qw - 1u); nx++) {
                    if (labels[(size_t)ny * qw + nx] != 0u) {
                        keep[scratch[i]] |= 2u;
                    }
                }
            }
        }
    }

    next = *count;
    for (label = 1; label <= found; label++) {
        keep[label] = (keep[label] == 1u) ? ++next : 0u;
    }

    for (i = 0; i < total; i++) {
        if (scratch[i] != 0u && keep[scratch[i]] != 0u) {
            labels[i] = keep[scratch[i]];
        }
    }

    *count = next;

    hdrg_mem_free(alloc, keep);

    return HDRG_OK;
}

/*
 * The regions' surround and judgment: their rings by a two-pass chamfer
 * distance transform from all regions at once (a cell belongs to its
 * nearest region, ties to the one the raster passes reach first), the first
 * ring's drops, the judgment of regions 1 to judge (the others keep theirs).
 * Regions of no cells are left out.
 */
static void hdrg_surround(const hdrg_lights_args_t *args, const float *cells,
                          const uint32_t *labels, uint32_t judge, hdrg_region_t *regions,
                          uint16_t *dist, uint32_t *owner)
{
    const uint32_t per = 1u << (HDRG_QUARTER_SHIFT - args->shift), qw = args->qw, qh = args->qh;
    const size_t total = (size_t)qw * qh;
    const float scale = args->scale, *qluma = cells, *qy = cells + 3u * total;
    hdrg_region_t *region;
    uint32_t label, x, y, k, nx, ny, step;
    int64_t tx, ty;
    size_t i, t, c;
    float vx, vy, len, last;

    for (label = 1; label <= judge; label++) {
        region = &regions[label];
        region->drop = 0.0;
        region->drop2 = 0.0;
        region->travel = 0.0;
        region->lean_x = 0.0;
        region->lean_y = 0.0;
        region->dir_x = 0.0;
        region->dir_y = 0.0;
        region->size = 0.0;
        region->outer = 0.0;
        region->outer_y = 0.0;
        region->around = 0.0f;
        region->rays = 0;
        region->judged = false;
        region->boost = 0.0f;
        region->lo = 0.0f;
        region->hi = 0.0f;
    }

    /* the seeded regions' cells are at distance 0; the rest of the frame is their surround */
    for (i = 0; i < total; i++) {
        dist[i] = HDRG_DIST_NONE;
        owner[i] = 0;

        if (labels[i] != 0 && regions[labels[i]].seeded) {
            dist[i] = 0;
            owner[i] = labels[i];
        }
    }

    for (y = 0; y < qh; y++) {
        for (x = 0; x < qw; x++) {
            i = (size_t)y * qw + x;

            if (x > 0) {
                hdrg_chamfer(dist, owner, i, i - 1u, 3u);
            }

            if (y > 0) {
                hdrg_chamfer(dist, owner, i, i - qw, 3u);
            }

            if (x > 0 && y > 0) {
                hdrg_chamfer(dist, owner, i, i - qw - 1u, 4u);
            }

            if (x + 1u < qw && y > 0) {
                hdrg_chamfer(dist, owner, i, i - qw + 1u, 4u);
            }
        }
    }

    for (y = qh; y-- > 0;) {
        for (x = qw; x-- > 0;) {
            i = (size_t)y * qw + x;

            if (x + 1u < qw) {
                hdrg_chamfer(dist, owner, i, i + 1u, 3u);
            }

            if (y + 1u < qh) {
                hdrg_chamfer(dist, owner, i, i + qw, 3u);
            }

            if (x + 1u < qw && y + 1u < qh) {
                hdrg_chamfer(dist, owner, i, i + qw + 1u, 4u);
            }

            if (x > 0 && y + 1u < qh) {
                hdrg_chamfer(dist, owner, i, i + qw - 1u, 4u);
            }
        }
    }

    /*
     * the first ring's outward drops: each cell against the cell REACH
     * farther along the sum of the directions from its region's neighbors
     */
    for (y = 0; y < qh; y++) {
        for (x = 0; x < qw; x++) {
            i = (size_t)y * qw + x;
            k = (dist[i] != HDRG_DIST_NONE) ? ((uint32_t)dist[i] + 1u) / 3u : UINT32_MAX;

            if (k != 1u) {
                continue;
            }

            region = &regions[owner[i]];
            vx = 0.0f;
            vy = 0.0f;

            for (ny = (y > 0u) ? y - 1u : 0u; ny <= HDRG_MIN(y + 1u, qh - 1u); ny++) {
                for (nx = (x > 0u) ? x - 1u : 0u; nx <= HDRG_MIN(x + 1u, qw - 1u); nx++) {
                    t = (size_t)ny * qw + nx;

                    if (dist[t] == 0u && owner[t] == owner[i]) {
                        vx += (float)x - (float)nx;
                        vy += (float)y - (float)ny;
                    }
                }
            }

            len = sqrtf(vx * vx + vy * vy);
            if (len < 0.5f) {
                continue;
            }

            tx = (int64_t)floorf((float)x + vx * HDRG_GLOW_REACH * (float)per / len + 0.5f);
            ty = (int64_t)floorf((float)y + vy * HDRG_GLOW_REACH * (float)per / len + 0.5f);
            if (tx < 0 || ty < 0 || tx >= (int64_t)qw || ty >= (int64_t)qh) {
                continue;
            }

            t = (size_t)ty * qw + (size_t)tx;
            if (labels[t] != 0u) {
                continue;
            }

            /* the cells along the ray, every per cells: its travel */
            for (step = 1, last = qy[i]; step <= (uint32_t)HDRG_GLOW_REACH; step++) {
                c = (size_t)(int64_t)floorf((float)y + vy * (float)step * (float)per / len + 0.5f) *
                        qw +
                    (size_t)(int64_t)floorf((float)x + vx * (float)step * (float)per / len + 0.5f);
                region->travel += (double)fabsf(last - qy[c]);
                last = qy[c];
            }

            region->drop += (double)(qy[i] - qy[t]);
            region->drop2 += (double)(qy[i] - qy[t]) * (double)(qy[i] - qy[t]);
            region->lean_x += (double)(qy[i] - qy[t]) * (double)(vx / len);
            region->lean_y += (double)(qy[i] - qy[t]) * (double)(vy / len);
            region->dir_x += (double)(vx / len);
            region->dir_y += (double)(vy / len);
            region->size += (double)fabsf(qy[i] - qy[t]);
            region->outer += (double)qluma[t];
            region->outer_y += (double)qy[t];
            region->rays++;
        }
    }

    /*
     * a small light stands out of its surround as a light does, not as a
     * bright spot of a white surface (Bonato & Gilchrist 1994: 1.7 x the
     * brightest surface around)
     */
    if (args->small > 0u) {
        for (i = 0; i < total; i++) {
            k = (dist[i] != HDRG_DIST_NONE) ? ((uint32_t)dist[i] + 1u) / 3u : UINT32_MAX;

            if (k >= 2u && k <= HDRG_LIGHT_RINGS * per) {
                regions[owner[i]].around = HDRG_MAX(regions[owner[i]].around, qy[i]);
            }
        }
    }

    for (label = 1; label <= judge; label++) {
        region = &regions[label];

        if (region->cells == 0) {
            continue;
        }

        if (args->small > 0u &&
            (region->cells > args->small ||
             (float)(region->luma / (double)region->cells) < HDRG_FINE_CONTRAST * region->around)) {
            continue;
        }

        hdrg_judge(scale, (float)(per * per), (float)total, region);
        region->judged = true;
    }
}

/*
 * the colored regions judged no light (below KEEP) left out, their cells
 * back to the surround as without the colored lights; false when there are
 * none
 */
static bool hdrg_colored_unlit(uint32_t white, uint32_t count, size_t total, uint32_t *labels,
                               hdrg_region_t *regions)
{
    uint32_t label;
    size_t i;
    bool any;

    any = false;
    for (label = white + 1u; label <= count; label++) {
        if (regions[label].cells != 0 && regions[label].boost < HDRG_COLORED_KEEP) {
            regions[label].cells = 0;
            regions[label].seeded = false;
            any = true;
        }
    }

    if (!any) {
        return false;
    }

    for (i = 0; i < total; i++) {
        if (labels[i] > white && regions[labels[i]].cells == 0) {
            labels[i] = 0;
        }
    }

    return true;
}

/*
 * The lights: components of the bright cells (and the colored ones), their
 * surround and judgment (hdrg_surround; again without the colored regions
 * judged no light, so a picture without a colored light renders as without
 * them), then the per-cell planes. Sequential: no value depends on the
 * thread count.
 */
static hdrg_status_t hdrg_lights(const hdrg_allocator_t *alloc, const hdrg_lights_args_t *args,
                                 const float *cells, hdrg_plane_t planes[3], uint32_t *lit,
                                 hdrg_light_list_t *list)
{
    const uint32_t qw = args->qw, qh = args->qh;
    const size_t total = (size_t)qw * qh;
    const float scale = args->scale, *qmax = cells + total, *qpeak = cells + 2u * total,
                *qy = cells + 3u * total, *qchroma = cells + 4u * total;
    hdrg_region_t *regions, *region;
    hdrg_status_t status;
    uint32_t *labels, *owner, count, white, label, x, y, k, lights;
    uint16_t *dist;
    uint8_t *mask;
    size_t i;

    labels = (uint32_t *)hdrg_mem_alloc(alloc, total * sizeof(uint32_t) * 2u);
    dist = (uint16_t *)hdrg_mem_alloc(alloc, total * sizeof(uint16_t));
    mask = (uint8_t *)hdrg_mem_alloc(alloc, total);
    if (labels == NULL || dist == NULL || mask == NULL) {
        hdrg_mem_free(alloc, labels);
        hdrg_mem_free(alloc, dist);
        hdrg_mem_free(alloc, mask);

        return HDRG_ERR_NOMEM;
    }

    owner = labels + total;

    for (i = 0; i < total; i++) {
        mask[i] = (qmax[i] >= HDRG_LIGHT_GROW * scale) ? 1u : 0u;
    }

    status = hdrg_label_components(alloc, mask, qw, qh, labels, &count);
    white = (status == HDRG_OK) ? count : 0u;

    /* owner is free until the distances */
    if (status == HDRG_OK && args->colored) {
        status = hdrg_colored_regions(alloc, args, qmax, qpeak, mask, labels, owner, &count);
    }

    if (status != HDRG_OK) {
        hdrg_mem_free(alloc, labels);
        hdrg_mem_free(alloc, dist);
        hdrg_mem_free(alloc, mask);

        return status;
    }

    regions = (hdrg_region_t *)hdrg_mem_calloc(alloc, (size_t)count + 1u, sizeof(hdrg_region_t));
    if (regions == NULL) {
        status = HDRG_ERR_NOMEM;

        goto cleanup;
    }

    for (y = 0; y < qh; y++) {
        for (x = 0; x < qw; x++) {
            i = (size_t)y * qw + x;
            region = &regions[labels[i]];

            if (labels[i] == 0) {
                continue;
            }

            if (region->cells == 0) {
                region->x0 = x;
                region->y0 = y;
                region->x1 = x;
                region->y1 = y;
                region->colored = labels[i] > white;
            }

            region->luma += (double)qy[i];
            region->chroma += (double)qchroma[i];
            region->top = HDRG_MAX(region->top, qmax[i]);
            region->peak = HDRG_MAX(region->peak, qpeak[i]);
            region->x0 = HDRG_MIN(region->x0, x);
            region->x1 = HDRG_MAX(region->x1, x);
            region->y0 = HDRG_MIN(region->y0, y);
            region->y1 = HDRG_MAX(region->y1, y);
            region->cells++;
            region->seeded = region->seeded || qpeak[i] >= HDRG_LIGHT_SEED * scale;
        }
    }

    hdrg_surround(args, cells, labels, count, regions, dist, owner);

    /* the colored lights keep their judgment */
    if (args->colored && hdrg_colored_unlit(white, count, total, labels, regions)) {
        hdrg_surround(args, cells, labels, white, regions, dist, owner);
    }

    /* the judged lights' list and, per cell, the light whose boost it holds */
    if (list != NULL) {
        list->count = 0;

        for (label = 1; label <= count; label++) {
            list->count += regions[label].judged ? 1u : 0u;
        }

        list->of = (uint32_t *)hdrg_mem_calloc(alloc, total, sizeof(uint32_t));
        list->lights =
            (hdrg_light_t *)hdrg_mem_calloc(alloc, (size_t)list->count + 1u, sizeof(hdrg_light_t));
        if (list->of == NULL || list->lights == NULL) {
            status = HDRG_ERR_NOMEM;

            goto cleanup;
        }

        list->count = 0;

        for (label = 1; label <= count; label++) {
            region = &regions[label];

            if (region->judged) {
                region->index = list->count;
                list->lights[list->count].x0 = region->x0;
                list->lights[list->count].y0 = region->y0;
                list->lights[list->count].x1 = region->x1;
                list->lights[list->count].y1 = region->y1;
                list->lights[list->count].boost = region->boost;
                list->count++;
            }
        }
    }

    status = hdrg_plane_alloc(alloc, qw, qh, &planes[0]);
    if (status == HDRG_OK) {
        status = hdrg_plane_alloc(alloc, qw, qh, &planes[1]);
    }

    if (status == HDRG_OK) {
        status = hdrg_plane_alloc(alloc, qw, qh, &planes[2]);
    }

    /* none of the planes kept on a failure */
    if (status != HDRG_OK) {
        for (i = 0; i < 3u; i++) {
            hdrg_plane_free(alloc, &planes[i]);
        }

        goto cleanup;
    }

    lights = 0;
    for (i = 0; i < total; i++) {
        region = &regions[owner[i]];
        k = (dist[i] != HDRG_DIST_NONE) ? ((uint32_t)dist[i] + 1u) / 3u : UINT32_MAX;
        planes[0].data[i] = (k <= args->zone) ? region->boost : 0.0f;
        planes[1].data[i] = region->lo;
        planes[2].data[i] = region->hi;
        lights += (k == 0 && region->boost >= 0.5f) ? 1u : 0u;

        if (list != NULL && k <= args->zone && region->judged) {
            list->of[i] = 1u + region->index;
        }
    }

    *lit = lights;

cleanup:
    hdrg_mem_free(alloc, regions);
    hdrg_mem_free(alloc, labels);
    hdrg_mem_free(alloc, dist);
    hdrg_mem_free(alloc, mask);

    return status;
}

/*
 * The lights of the cells (hdrg_lights): on 4 x 4 cells as they are; on
 * 2 x 2 cells (QUALITY) the 4 x 4 judgment of their sums, and where they take
 * more, the small lights the finer cells resolve (HDRG_FINE_SMALL cells at
 * most, a sparkle a 4 x 4 cell blurs, their boost within HDRG_FINE_ZONE rings):
 * judging every light on the finer cells takes a white window frame's edge
 * for a glow. Colored lights only on the 4 x 4 cells.
 */
static hdrg_status_t hdrg_analyze_lights(const hdrg_allocator_t *alloc, float scale, uint8_t shift,
                                         uint32_t width, uint32_t height, uint32_t qw, uint32_t qh,
                                         const float *cells, bool listed, bool colored,
                                         hdrg_analysis_t *out)
{
    const size_t total = (size_t)qw * qh;
    hdrg_lights_args_t args;
    hdrg_plane_t coarse[3], fine[3], merged[3];
    hdrg_light_list_t whole, coarse_list, fine_list;
    hdrg_status_t status;
    uint32_t cw, ch, x, y, lit, unused, pixels, k;
    uint8_t p;
    size_t i, c, coarse_total;
    float *sums, *grid;

    memset(coarse, 0, sizeof(coarse));
    memset(fine, 0, sizeof(fine));
    memset(merged, 0, sizeof(merged));
    memset(&whole, 0, sizeof(whole));
    memset(&coarse_list, 0, sizeof(coarse_list));
    memset(&fine_list, 0, sizeof(fine_list));
    args.scale = scale;
    args.small = 0;
    args.colored = colored;

    if (shift == HDRG_QUARTER_SHIFT) {
        args.qw = qw;
        args.qh = qh;
        args.zone = HDRG_LIGHT_RINGS;
        args.shift = shift;

        status = hdrg_lights(alloc, &args, cells, merged, &lit, listed ? &whole : NULL);
        if (status == HDRG_OK) {
            out->light = merged[0];
            out->light_lo = merged[1];
            out->light_hi = merged[2];
            out->light_ratio = (float)lit / (float)total;
            out->light_of = whole.of;
            out->lights = whole.lights;
            out->light_count = whole.count;
        } else {
            hdrg_mem_free(alloc, whole.of);
            hdrg_mem_free(alloc, whole.lights);
        }

        return status;
    }

    /* the 4 x 4 cells' sums of La and Y over their pixels, maxima, then the means */
    cw = (qw + 1u) / 2u;
    ch = (qh + 1u) / 2u;
    coarse_total = (size_t)cw * ch;
    grid = (float *)hdrg_mem_calloc(alloc, coarse_total * 6u, sizeof(float));
    if (grid == NULL) {
        return HDRG_ERR_NOMEM;
    }

    sums = grid + 5u * coarse_total;

    for (y = 0; y < qh; y++) {
        for (x = 0; x < qw; x++) {
            i = (size_t)y * qw + x;
            c = (size_t)(y >> 1) * cw + (x >> 1);
            pixels = HDRG_MIN(2u, width - 2u * x) * HDRG_MIN(2u, height - 2u * y);
            grid[c] += cells[i] * (float)pixels;

            if (cells[total + i] > grid[coarse_total + c]) {
                grid[4u * coarse_total + c] = cells[4u * total + i];
            }

            grid[coarse_total + c] = HDRG_MAX(grid[coarse_total + c], cells[total + i]);
            grid[2u * coarse_total + c] =
                HDRG_MAX(grid[2u * coarse_total + c], cells[2u * total + i]);
            grid[3u * coarse_total + c] += cells[3u * total + i] * (float)pixels;
            sums[c] += (float)pixels;
        }
    }

    for (c = 0; c < coarse_total; c++) {
        grid[c] /= sums[c];
        grid[3u * coarse_total + c] /= sums[c];
    }

    args.qw = cw;
    args.qh = ch;
    args.zone = HDRG_LIGHT_RINGS;
    args.shift = HDRG_QUARTER_SHIFT;

    status = hdrg_lights(alloc, &args, grid, coarse, &lit, listed ? &coarse_list : NULL);

    if (status == HDRG_OK) {
        args.qw = qw;
        args.qh = qh;
        args.small = HDRG_FINE_SMALL;
        args.zone = HDRG_FINE_ZONE;
        args.shift = shift;
        args.colored = false;

        status = hdrg_lights(alloc, &args, cells, fine, &unused, listed ? &fine_list : NULL);
    }

    for (p = 0; status == HDRG_OK && p < 3u; p++) {
        status = hdrg_plane_alloc(alloc, qw, qh, &merged[p]);
    }

    /* the lists: the coarse lights' boxes in fine cells, then the small lights */
    if (status == HDRG_OK && listed) {
        whole.count = coarse_list.count + fine_list.count;
        whole.of = (uint32_t *)hdrg_mem_calloc(alloc, total, sizeof(uint32_t));
        whole.lights =
            (hdrg_light_t *)hdrg_mem_calloc(alloc, (size_t)whole.count + 1u, sizeof(hdrg_light_t));
        status = (whole.of != NULL && whole.lights != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && listed) {
        for (k = 0; k < coarse_list.count; k++) {
            whole.lights[k] = coarse_list.lights[k];
            whole.lights[k].x0 = 2u * coarse_list.lights[k].x0;
            whole.lights[k].y0 = 2u * coarse_list.lights[k].y0;
            whole.lights[k].x1 = HDRG_MIN(2u * coarse_list.lights[k].x1 + 1u, qw - 1u);
            whole.lights[k].y1 = HDRG_MIN(2u * coarse_list.lights[k].y1 + 1u, qh - 1u);
        }

        for (k = 0; k < fine_list.count; k++) {
            whole.lights[coarse_list.count + k] = fine_list.lights[k];
        }
    }

    /* each fine cell: the coarse light over it, or the small light that takes more */
    for (y = 0; status == HDRG_OK && y < qh; y++) {
        for (x = 0; x < qw; x++) {
            i = (size_t)y * qw + x;
            c = (size_t)(y >> 1) * cw + (x >> 1);

            for (p = 0; p < 3u; p++) {
                merged[p].data[i] =
                    (fine[0].data[i] > coarse[0].data[c]) ? fine[p].data[i] : coarse[p].data[c];
            }

            if (listed) {
                whole.of[i] =
                    (fine[0].data[i] > coarse[0].data[c])
                        ? ((fine_list.of[i] != 0u) ? coarse_list.count + fine_list.of[i] : 0u)
                        : coarse_list.of[c];
            }
        }
    }

    if (status == HDRG_OK) {
        out->light = merged[0];
        out->light_lo = merged[1];
        out->light_hi = merged[2];
        out->light_ratio = (float)lit / (float)coarse_total;
        out->light_of = whole.of;
        out->lights = whole.lights;
        out->light_count = whole.count;
    } else {
        for (p = 0; p < 3u; p++) {
            hdrg_plane_free(alloc, &merged[p]);
        }

        hdrg_mem_free(alloc, whole.of);
        hdrg_mem_free(alloc, whole.lights);
    }

    hdrg_mem_free(alloc, coarse_list.of);
    hdrg_mem_free(alloc, coarse_list.lights);
    hdrg_mem_free(alloc, fine_list.of);
    hdrg_mem_free(alloc, fine_list.lights);

    for (p = 0; p < 3u; p++) {
        hdrg_plane_free(alloc, &coarse[p]);
        hdrg_plane_free(alloc, &fine[p]);
    }

    hdrg_mem_free(alloc, grid);

    return status;
}

extern hdrg_status_t hdrg_label_components(const hdrg_allocator_t *alloc, const uint8_t *mask,
                                           uint32_t width, uint32_t height, uint32_t *labels,
                                           uint32_t *count)
{
    uint32_t *parent, *remap, next, left, up, root, compact, x, y;
    size_t idx, total;

    total = (size_t)width * height;

    parent = (uint32_t *)hdrg_mem_alloc(alloc, (total + 1) * sizeof(uint32_t));
    if (parent == NULL) {
        return HDRG_ERR_NOMEM;
    }

    next = 1;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            idx = (size_t)y * width + x;
            if (!mask[idx]) {
                labels[idx] = 0;

                continue;
            }

            left = (x > 0) ? labels[idx - 1] : 0;
            up = (y > 0) ? labels[idx - width] : 0;
            if (left == 0 && up == 0) {
                parent[next] = next;
                labels[idx] = next;
                next++;
            } else if (left != 0 && up == 0) {
                labels[idx] = left;
            } else if (left == 0) {
                labels[idx] = up;
            } else {
                labels[idx] = HDRG_MIN(left, up);

                hdrg_uf_union(parent, left, up);
            }
        }
    }

    remap = (uint32_t *)hdrg_mem_calloc(alloc, next, sizeof(uint32_t));
    if (remap == NULL) {
        hdrg_mem_free(alloc, parent);

        return HDRG_ERR_NOMEM;
    }

    compact = 0;
    for (idx = 0; idx < total; idx++) {
        if (labels[idx] == 0) {
            continue;
        }

        root = hdrg_uf_find(parent, labels[idx]);
        if (remap[root] == 0) {
            compact++;
            remap[root] = compact;
        }

        labels[idx] = remap[root];
    }

    *count = compact;

    hdrg_mem_free(alloc, parent);
    hdrg_mem_free(alloc, remap);

    return HDRG_OK;
}

extern hdrg_status_t hdrg_analyze(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                  const hdrg_source_t *src, const hdrg_analyze_opts_t *opts,
                                  hdrg_analysis_t *out)
{
    const uint8_t shift = (opts != NULL && opts->cell_shift == HDRG_FINE_SHIFT)
                              ? HDRG_FINE_SHIFT
                              : HDRG_QUARTER_SHIFT;
    const float scale = (opts != NULL) ? HDRG_CLAMP(opts->bright_scale, 1e-3f, 1.0f) : 1.0f;
    hdrg_stats_job_t stats_job;
    hdrg_status_t status;
    uint64_t *counts, *hist, cumulative, target_count;
    uint32_t qw, qh, bin, label, concurrency, worker;
    size_t total, qtotal;
    bool listed;
    double key_log;
    float g, at, w, share, *cells, *scratch;

    memset(out, 0, sizeof(*out));
    counts = NULL;
    scratch = NULL;

    if (opts != NULL && opts->bars) {
        if (opts->given != NULL) {
            memcpy(out->bars, opts->given->bars, sizeof(out->bars));
        } else {
            hdrg_bars(src, out->bars);
        }
    }

    /* the statistics cover what is inside the bars */
    total = (size_t)(src->width - out->bars[2] - out->bars[3]) *
            (src->height - out->bars[0] - out->bars[1]);
    qw = (src->width + (1u << shift) - 1u) >> shift;
    qh = (src->height + (1u << shift) - 1u) >> shift;
    qtotal = (size_t)qw * qh;
    concurrency = hdrg_exec_concurrency(exec);

    out->cell_shift = shift;

    memset(&stats_job, 0, sizeof(stats_job));
    stats_job.src = src;
    stats_job.shift = shift;
    stats_job.qw = qw;
    stats_job.x0 = out->bars[2];
    stats_job.x1 = src->width - out->bars[3];
    stats_job.y0 = out->bars[0];
    stats_job.y1 = src->height - out->bars[1];
    stats_job.scratch_stride = (size_t)src->width * 4u + (size_t)qw * HDRG_CELL_SUMS;

    /* (a very wide picture's rows: no product wraps in 32 bits) */
    if (stats_job.scratch_stride > SIZE_MAX / sizeof(float) / concurrency) {
        return HDRG_ERR_NOMEM;
    }

    counts =
        (uint64_t *)hdrg_mem_calloc(alloc, (size_t)concurrency * HDRG_HIST_BINS, sizeof(uint64_t));
    scratch = (float *)hdrg_mem_alloc(alloc, (size_t)concurrency * stats_job.scratch_stride *
                                                 sizeof(float));
    cells = (float *)hdrg_mem_alloc(alloc, qtotal * HDRG_ANALYZE_CELLS * sizeof(float));
    if (counts == NULL || scratch == NULL || cells == NULL) {
        status = HDRG_ERR_NOMEM;

        goto cleanup;
    }

    stats_job.counts = counts;
    stats_job.scratch = scratch;
    stats_job.qluma = cells;
    stats_job.qmax = cells + qtotal;
    stats_job.qpeak = cells + 2u * qtotal;
    stats_job.qy = cells + 3u * qtotal;
    stats_job.qchroma = cells + 4u * qtotal;

    /* the sums given (a GPU's), else from the pixels */
    if (opts != NULL && opts->given != NULL) {
        memcpy(cells, opts->given->cells, qtotal * HDRG_ANALYZE_CELLS * sizeof(float));
        memcpy(counts, opts->given->hist, HDRG_HIST_BINS * sizeof(uint64_t));
    } else {
        hdrg_exec_worker_rows(exec, src->height, hdrg_stats_rows, &stats_job);
    }

    hist = counts;
    for (worker = 1; worker < concurrency; worker++) {
        for (bin = 0; bin < HDRG_HIST_BINS; bin++) {
            hist[bin] += counts[(size_t)worker * HDRG_HIST_BINS + bin];
        }
    }

    /* histogram: p99.5 */
    cumulative = 0;
    target_count = (uint64_t)((double)total * 0.995);
    out->p995_luma = 1.0f;
    for (bin = 0; bin < HDRG_HIST_BINS; bin++) {
        cumulative += hist[bin];
        if (cumulative >= target_count) {
            out->p995_luma = (float)bin / (float)(HDRG_HIST_BINS - 1);

            break;
        }
    }

    /* key: geometric mean of La from the histogram (bin centers, floored) */
    key_log = 0.0;
    for (bin = 0; bin < HDRG_HIST_BINS; bin++) {
        if (hist[bin] != 0) {
            key_log += (double)hist[bin] *
                       (double)hdrg_math_log2f(HDRG_MAX(
                           ((float)bin + 0.5f) / (float)(HDRG_HIST_BINS - 1), HDRG_KEY_FLOOR));
        }
    }

    out->key_luma = hdrg_math_exp2f((float)(key_log / (double)total));

    /*
     * the scene histogram: each linear bin's center shared between the two
     * bins of log2 La around it by its place between their centers, so a
     * level moving across a bin's edge (a fade of a flat picture) moves its
     * share gradually, as the scene-cut test's shift does
     */
    for (bin = 0; bin < HDRG_HIST_BINS; bin++) {
        g = hdrg_math_log2f(HDRG_MAX(((float)bin + 0.5f) / (float)(HDRG_HIST_BINS - 1), 1e-6f));
        at = HDRG_CLAMP((g + HDRG_SCENE_STOPS) * ((float)HDRG_SCENE_BINS / HDRG_SCENE_STOPS) - 0.5f,
                        0.0f, (float)(HDRG_SCENE_BINS - 1u));
        label = (uint32_t)at;
        w = at - (float)label;
        share = (float)((double)hist[bin] / (double)total);
        out->scene_hist[label] += share * (1.0f - w);

        if (label + 1u < HDRG_SCENE_BINS) {
            out->scene_hist[label + 1u] += share * w;
        }
    }

    /* the lights listed for the predictor too (its features per light), dropped after it */
    listed = opts != NULL &&
             (opts->lights || opts->features || (opts->predictor > 0.0f && opts->judge != NULL));
    status = hdrg_analyze_lights(alloc, scale, shift, src->width, src->height, qw, qh, cells,
                                 listed, opts != NULL && opts->colored, out);

    if (status == HDRG_OK && listed &&
        (opts->features || (opts->predictor > 0.0f && opts->judge != NULL))) {
        status = hdrg_predict_lights(alloc, out, src, cells, qw, qh, shift, opts);
    }

    if (status == HDRG_OK && listed && !opts->lights && !opts->features) {
        hdrg_mem_free(alloc, out->light_of);
        hdrg_mem_free(alloc, out->lights);
        out->light_of = NULL;
        out->lights = NULL;
        out->light_count = 0;
    }

    if (status == HDRG_OK && opts != NULL && opts->lights) {
        status = hdrg_plane_alloc(alloc, qw, qh, &out->luma);
    }

    if (status == HDRG_OK && opts != NULL && opts->lights) {
        memcpy(out->luma.data, cells + 3u * qtotal, qtotal * sizeof(float));
    }

cleanup:
    hdrg_mem_free(alloc, counts);
    hdrg_mem_free(alloc, scratch);
    hdrg_mem_free(alloc, cells);

    if (status != HDRG_OK) {
        hdrg_analysis_free(alloc, out);
    }

    return status;
}

extern void hdrg_analysis_free(const hdrg_allocator_t *alloc, hdrg_analysis_t *analysis)
{
    if (analysis == NULL) {
        return;
    }

    hdrg_plane_free(alloc, &analysis->light);
    hdrg_plane_free(alloc, &analysis->light_lo);
    hdrg_plane_free(alloc, &analysis->light_hi);
    hdrg_plane_free(alloc, &analysis->luma);
    hdrg_mem_free(alloc, analysis->light_of);
    hdrg_mem_free(alloc, analysis->lights);
    hdrg_mem_free(alloc, analysis->light_features);

    analysis->light_of = NULL;
    analysis->lights = NULL;
    analysis->light_features = NULL;
    analysis->light_count = 0;
}

extern void hdrg_analyze_bars(const float *rows, const float *columns, uint32_t width,
                              uint32_t height, uint32_t bars[4])
{
    memset(bars, 0, 4u * sizeof(uint32_t));

    while (bars[0] < height && rows[bars[0]] < HDRG_BAR_LIGHT) {
        bars[0]++;
    }

    while (bars[1] < height - bars[0] && rows[height - 1u - bars[1]] < HDRG_BAR_LIGHT) {
        bars[1]++;
    }

    while (bars[2] < width && columns[bars[2]] < HDRG_BAR_LIGHT) {
        bars[2]++;
    }

    while (bars[3] < width - bars[2] && columns[width - 1u - bars[3]] < HDRG_BAR_LIGHT) {
        bars[3]++;
    }

    if ((uint64_t)(width - bars[2] - bars[3]) * (height - bars[0] - bars[1]) * 4u <
        (uint64_t)width * height) {
        memset(bars, 0, 4u * sizeof(uint32_t));
    }
}
