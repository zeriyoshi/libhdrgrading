/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "parts.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"

#define HDRG_LIT(s)              s, sizeof(s) - 1u
#define HDRG_ST_ID               "hdrg.stats.temporal"
#define HDRG_ST_LICENSE          "Apache-2.0 OR MIT"
#define HDRG_ST_COPY             "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_ST_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define HDRG_ST_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define HDRG_ST_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

#define HDRG_ST_KEY_FLOOR 0.001f /* the analysis' key floor (a black frame's key) */

/*
 * A fade-out: a run of frames whose log2 key and p99.5 fall together (each
 * by STEP at least, within SPREAD of each other and SLACK), at most REACH
 * seconds of them (MAX frames)
 */
#define HDRG_ST_FADE_STEP   0.02f /* stops */
#define HDRG_ST_FADE_SPREAD 0.5f
#define HDRG_ST_FADE_SLACK  0.05f /* stops */
#define HDRG_ST_FADE_STEPS  3u
#define HDRG_ST_FADE_REACH  2.0f /* seconds: the frames a run counts back at most */
#define HDRG_ST_FADE_MAX    480u
#define HDRG_ST_FADE_SLOTS                                                                         \
    (HDRG_ST_FADE_MAX + 1u) /* the frames a run reaches back to, by index                          \
                             */

/* RECURSIVE: the EMA's time constants, its state's grid and the preroll it settles in */
#define HDRG_ST_RECURSIVE_ATTACK 0.15f /* seconds */
#define HDRG_ST_RECURSIVE_DECAY  0.6f
#define HDRG_ST_RECURSIVE_STEPS  1024.0f /* per stop */
#define HDRG_ST_RECURSIVE_SETTLE 6.0f    /* time constants (the slower) its preroll takes */

/*
 * The lights' judgments, steadied: each light's boost is the median of its
 * own and the boosts at its place in the frames of the statistics window
 * around it in the scene, up to STEADY_FRAMES before and after it (WINDOWED:
 * its past and future; RECURSIVE: STEADY_FRAMES before), the lower of the two
 * middle ones (in doubt the smaller boost). A frame votes with its light whose
 * center is the nearest to the light's, within STEADY_MOVE pixels a frame away
 * (its motion); a frame without one there does not vote (a light coming into
 * view keeps its own judgment)
 */
#define HDRG_ST_STEADY_FRAMES 8u
#define HDRG_ST_STEADY_MOVE   8u

/*
 * OFFLINE (a file nobody watches: no one waits for the frames) steadies a
 * WINDOWED session's lights over STEADY_LONG frames on each side, whatever
 * the statistics' window, place by place: each cell near a light takes the
 * median of the boosts its place had in those frames as judged (1/254 steps;
 * a frame with no light near the place does not vote), the place followed
 * with the camera: between each frame and the one before, the translation of
 * their cells' mean Y (each over its mean, a fade's change aside) that
 * matches best (MOTION_REACH cells each way on a grid of 2 x 2 cells, then
 * the cells around it, then a parabola for the fraction), summed over the
 * frames between two and rounded to a cell. A place keeps the same frames
 * from one frame to the next, so its median does not swing with how the
 * frame happens to cut its lights into regions
 */
#define HDRG_ST_STEADY_LONG   24u
#define HDRG_ST_STEADY_VALUES (2u * HDRG_ST_STEADY_LONG + 1u)
#define HDRG_ST_STEADY_STEPS  254.0f
#define HDRG_ST_MOTION_REACH  8
#define HDRG_ST_MOTION_SHIFTS ((2u * HDRG_ST_MOTION_REACH + 1u) * (2u * HDRG_ST_MOTION_REACH + 1u))
#define HDRG_ST_MOTION_LEVELS 4u   /* the pyramid's levels above the grid at most ... */
#define HDRG_ST_MOTION_COARSE 256u /* ... its top the first no wider than this (cells) */
#define HDRG_ST_MOTION_BLOCKS 16u  /* a shift's cost summed in this many blocks of rows */
#define HDRG_ST_MOVES         128u /* the sent frames' motion, by index (the queue's and more) */

typedef struct {
    float anchor, key, fit; /* log2 */
} hdrg_st_stat_t;

typedef struct {
    uint32_t run;
    float dim;
} hdrg_st_fade_t;

typedef struct {
    uint32_t track;
    float boost;
} hdrg_st_vote_t;

/* the instance: one session's statistics */
typedef struct {
    hdrg_stats_session_t info;
    hdrg_st_stat_t history[HDRG_SESSION_MAX_WINDOW]; /* the scene's latest rendered frames */
    hdrg_st_stat_t ema;                              /* RECURSIVE: the state (log2) */
    hdrg_session_pass_t *passes;   /* TWO_PASS: the scene means of the first pass, per frame */
    hdrg_session_light_t *tracked; /* TWO_PASS: the first pass's lights, each its track's
                                      boost (a frame's from tracked_at[i] to tracked_at[i + 1]) */
    uint32_t *tracked_at;
    hdrg_st_fade_t *fades; /* TWO_PASS, second pass: per frame, from the first pass's */
    hdrg_stats_light_t *steady[HDRG_ST_STEADY_LONG + 1u]; /* the latest rendered frames' lights as
                                                             judged (the latest first), then the
                                                             frame's own ... */
    uint32_t steady_count[HDRG_ST_STEADY_LONG + 1u];      /* ... how many ... */
    uint32_t steady_cap[HDRG_ST_STEADY_LONG + 1u];        /* ... room for ... */
    uint32_t steady_index[HDRG_ST_STEADY_LONG];           /* ... their frames (UINT32_MAX: none) */
    float steady_move[HDRG_ST_STEADY_LONG][2];            /* ... and their motion (OFFLINE) */
    uint8_t *steady_cells[HDRG_ST_STEADY_LONG + 1u]; /* OFFLINE: the same frames' cells' boosts as
                                                        judged (0: no light near, else 1 + the
                                                        boost in 1/254), in steady_block */
    uint8_t *steady_block;
    uint32_t steady_w, steady_h; /* ... their cells */
    uint32_t steady_frames;      /* the frames each way (OFFLINE: STEADY_LONG) */
    float *motion_prev; /* the latest sent frame's cells' Y over their mean (NULL: none), then the
                           scratch: the frame's, the 2 x 2 grids */
    uint32_t motion_w, motion_h;
    uint32_t move_index[HDRG_ST_MOVES]; /* the sent frames' motion from the frame before (cells),
                                           by index (UINT32_MAX: none) */
    float moves[HDRG_ST_MOVES][2];
    float p995s[HDRG_ST_FADE_SLOTS]; /* the sent frames' p99.5, by index */
    uint32_t history_count, history_next, fade_run, fade_reach;
    float fade_key, fade_anchor; /* the latest sent frame's (log2) */
    float attack, decay;         /* RECURSIVE: the EMA's steps a frame */
    bool steady_place;           /* the steadying by place (cells) and the motion: PLAYBACK and
                                    OFFLINE */
    bool steady_long;            /* OFFLINE: the long window */
    bool head_cut;               /* the frame rendering's scene cut */
} hdrg_st_t;

/* a RECURSIVE state on its 1/1024-stop grid */
static inline float hdrg_st_quantize(float v)
{
    return (float)(int32_t)(v * HDRG_ST_RECURSIVE_STEPS + ((v >= 0.0f) ? 0.5f : -0.5f)) /
           HDRG_ST_RECURSIVE_STEPS;
}

/* one EMA step of state toward x: by up when x is above, by down otherwise */
static inline float hdrg_st_ema(float state, float x, float up, float down)
{
    return hdrg_st_quantize(state + ((x > state) ? up : down) * (x - state));
}

/* a step of a fade-out: the log2 key and p99.5 falling together by dk and da */
static inline bool hdrg_st_darkening(float dk, float da)
{
    return dk <= -HDRG_ST_FADE_STEP && da < 0.0f &&
           fabsf(da - dk) <= HDRG_ST_FADE_SPREAD * fabsf(dk) + HDRG_ST_FADE_SLACK;
}

/* a judged light's box in the frames' pixels, its cells 2^shift pixels across */
static inline void hdrg_st_light(const hdrg_stats_light_t *light, uint8_t shift, uint32_t width,
                                 uint32_t height, hdrg_session_light_t *out)
{
    out->x0 = HDRG_ST_MIN(light->x0 << shift, width - 1u);
    out->y0 = HDRG_ST_MIN(light->y0 << shift, height - 1u);
    out->x1 = HDRG_ST_MIN(((light->x1 + 1u) << shift) - 1u, width - 1u);
    out->y1 = HDRG_ST_MIN(((light->y1 + 1u) << shift) - 1u, height - 1u);
    out->boost = light->boost;
}

/* the motion of the sent frame index from the frame before (0 for one not sent) */
static inline void hdrg_st_move(const hdrg_st_t *st, uint32_t index, float *x, float *y)
{
    const uint32_t slot = index % HDRG_ST_MOVES;

    *x = (st->move_index[slot] == index) ? st->moves[slot][0] : 0.0f;
    *y = (st->move_index[slot] == index) ? st->moves[slot][1] : 0.0f;
}

/*
 * the boost of the light of lights (count) whose center is the nearest to
 * light's, within reach cells; -1 = none
 */
static float hdrg_st_vote(const hdrg_stats_light_t *lights, uint32_t count,
                          const hdrg_stats_light_t *light, uint32_t reach)
{
    const int64_t cx = (int64_t)light->x0 + light->x1, cy = (int64_t)light->y0 + light->y1;
    uint32_t k;
    int64_t dx, dy, best;
    float boost;

    /* centers in half cells: the reach doubled */
    best = 4 * (int64_t)reach * reach + 1;
    boost = -1.0f;

    for (k = 0; k < count; k++) {
        dx = (int64_t)lights[k].x0 + lights[k].x1 - cx;
        dy = (int64_t)lights[k].y0 + lights[k].y1 - cy;

        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            boost = lights[k].boost;
        }
    }

    return boost;
}

/*
 * The overlap of b shifted by (sx, sy) with a, two w x h grids (false when
 * under half of them: the cost FLT_MAX)
 */
static bool hdrg_st_overlap(uint32_t w, uint32_t h, int32_t sx, int32_t sy, int32_t *x0,
                            int32_t *x1, int32_t *y0, int32_t *y1)
{
    *x0 = HDRG_ST_MAX(0, -sx);
    *x1 = HDRG_ST_MIN((int32_t)w, (int32_t)w - sx);
    *y0 = HDRG_ST_MAX(0, -sy);
    *y1 = HDRG_ST_MIN((int32_t)h, (int32_t)h - sy);

    return *x1 > *x0 && *y1 > *y0 && 2u * (uint32_t)((*x1 - *x0) * (*y1 - *y0)) >= w * h;
}

/*
 * The mean |b - a| over the overlap of count shifts (FLT_MAX: too little
 * overlap): each shift's sum of |b(x + sx, y + sy) - a(x, y)| in MOTION_BLOCKS
 * blocks of its overlap's rows, the blocks' sums added in order
 */
static void hdrg_st_costs(const float *a, const float *b, uint32_t w, uint32_t h,
                          const int32_t *shifts, uint32_t count, float *costs)
{
    uint32_t i, k, rows;
    int32_t sx, sy, x0, x1, y0, y1, x, y, ya, yb;
    double block, sum;

    for (i = 0; i < count; i++) {
        sx = shifts[2u * i];
        sy = shifts[2u * i + 1u];

        if (!hdrg_st_overlap(w, h, sx, sy, &x0, &x1, &y0, &y1)) {
            costs[i] = FLT_MAX;

            continue;
        }

        rows = (uint32_t)(y1 - y0);
        sum = 0.0;

        for (k = 0; k < HDRG_ST_MOTION_BLOCKS; k++) {
            ya = y0 + (int32_t)((uint64_t)rows * k / HDRG_ST_MOTION_BLOCKS);
            yb = y0 + (int32_t)((uint64_t)rows * (k + 1u) / HDRG_ST_MOTION_BLOCKS);
            block = 0.0;

            for (y = ya; y < yb; y++) {
                for (x = x0; x < x1; x++) {
                    block += (double)fabsf(b[(size_t)(y + sy) * w + (size_t)(x + sx)] -
                                           a[(size_t)y * w + (size_t)x]);
                }
            }

            sum += block;
        }

        costs[i] = (float)(sum / (double)((x1 - x0) * (y1 - y0)));
    }
}

/* the fraction of a parabola's minimum through the costs at -1, 0, 1 (within half) */
static float hdrg_st_fraction(float before, float at, float after)
{
    const float curve = before - 2.0f * at + after;

    if (!(curve > 0.0f) || before == FLT_MAX || after == FLT_MAX) {
        return 0.0f;
    }

    return HDRG_ST_CLAMP(0.5f * (before - after) / curve, -0.5f, 0.5f);
}

/*
 * OFFLINE: the frame's camera motion from the frame sent before (its cells'
 * Y in st->motion_prev), then the frame's grid kept for the next. A pyramid
 * of 2 x 2 means, its top the first no wider than COARSE cells (one level
 * below the grid at least: a QUALITY grid's 2-pixel cells take more): the
 * shifts within REACH there, then 3 x 3 around twice the choice on each level
 * down, then the cross around it on the grid (the fraction)
 */
static hdrg_status_t hdrg_st_motion(hdrg_st_t *st, const hdrg_stats_frame_t *frame,
                                    const hdrg_plugin_api_t *api, float *move_x, float *move_y)
{
    const uint32_t w = frame->width, h = frame->height;
    const size_t cells = (size_t)w * h;
    const float *grid = frame->luma;
    uint32_t x, y, n, k, widths[HDRG_ST_MOTION_LEVELS + 1u], heights[HDRG_ST_MOTION_LEVELS + 1u],
        top;
    uint8_t l;
    int32_t sx, sy, bx, by, shifts[2u * HDRG_ST_MOTION_SHIFTS];
    size_t i, total;
    bool kept;
    double mean;
    float *prev, *cur, *src, *dst, *levels_prev[HDRG_ST_MOTION_LEVELS + 1u],
        *levels_cur[HDRG_ST_MOTION_LEVELS + 1u], best, costs[3][3], found[HDRG_ST_MOTION_SHIFTS];

    *move_x = 0.0f;
    *move_y = 0.0f;

    if (grid == NULL || w / 2u == 0u || h / 2u == 0u) {
        return HDRG_OK;
    }

    widths[0] = w;
    heights[0] = h;
    total = cells;
    top = 1;

    for (l = 1; l <= HDRG_ST_MOTION_LEVELS; l++) {
        widths[l] = widths[l - 1u] / 2u;
        heights[l] = heights[l - 1u] / 2u;
        total += (widths[l] > 0u && heights[l] > 0u) ? (size_t)widths[l] * heights[l] : 0u;
    }

    while (top < HDRG_ST_MOTION_LEVELS && widths[top] > HDRG_ST_MOTION_COARSE &&
           widths[top + 1u] > 0u && heights[top + 1u] > 0u) {
        top++;
    }

    if (st->motion_prev == NULL || st->motion_w != w || st->motion_h != h) {
        api->mem_free(api, st->motion_prev);
        st->motion_prev = (float *)api->mem_alloc(api, 2u * total * sizeof(float));
        if (st->motion_prev == NULL) {
            return HDRG_ERR_NOMEM;
        }

        st->motion_w = w;
        st->motion_h = h;
        kept = false;
    } else {
        kept = !frame->cut;
    }

    prev = st->motion_prev;
    cur = prev + cells;
    levels_prev[0] = prev;
    levels_cur[0] = cur;
    levels_prev[1] = cur + cells;

    for (l = 1; l <= top; l++) {
        levels_cur[l] = levels_prev[l] + (size_t)widths[l] * heights[l];

        if (l < top) {
            levels_prev[l + 1u] = levels_cur[l] + (size_t)widths[l] * heights[l];
        }
    }

    /* the cells over their mean: a fade's change of the level aside */
    mean = 0.0;
    for (i = 0; i < cells; i++) {
        mean += (double)grid[i];
    }

    mean = HDRG_ST_MAX(mean / (double)cells, 1e-4);

    for (i = 0; i < cells; i++) {
        cur[i] = (float)((double)grid[i] / mean);
    }

    for (l = 1; kept && l <= top; l++) {
        for (k = 0; k < 2u; k++) {
            src = (k == 0u) ? levels_prev[l - 1u] : levels_cur[l - 1u];
            dst = (k == 0u) ? levels_prev[l] : levels_cur[l];

            for (y = 0; y < heights[l]; y++) {
                for (x = 0; x < widths[l]; x++) {
                    i = (size_t)(2u * y) * widths[l - 1u] + 2u * x;
                    dst[(size_t)y * widths[l] + x] =
                        0.25f * (src[i] + src[i + 1u] + src[i + widths[l - 1u]] +
                                 src[i + widths[l - 1u] + 1u]);
                }
            }
        }
    }

    if (kept) {
        /* the top level, no motion first (a flat frame stays put), then the cells around it */
        n = 0;

        for (sy = -HDRG_ST_MOTION_REACH; sy <= HDRG_ST_MOTION_REACH; sy++) {
            for (sx = -HDRG_ST_MOTION_REACH; sx <= HDRG_ST_MOTION_REACH; sx++) {
                shifts[2u * n] = sx;
                shifts[2u * n + 1u] = sy;
                n++;
            }
        }

        hdrg_st_costs(levels_prev[top], levels_cur[top], widths[top], heights[top], shifts, n,
                      found);
        bx = 0;
        by = 0;
        best = found[n / 2u];

        for (k = 0; k < n; k++) {
            if (found[k] < best) {
                best = found[k];
                bx = shifts[2u * k];
                by = shifts[2u * k + 1u];
            }
        }

        /* each level down: the 3 x 3 around twice the choice */
        for (l = (uint8_t)top; l > 0u; l--) {
            n = 0;

            for (sy = -1; sy <= 1; sy++) {
                for (sx = -1; sx <= 1; sx++) {
                    shifts[2u * n] = 2 * bx + sx;
                    shifts[2u * n + 1u] = 2 * by + sy;
                    n++;
                }
            }

            hdrg_st_costs(levels_prev[l - 1u], levels_cur[l - 1u], widths[l - 1u], heights[l - 1u],
                          shifts, n, found);
            sx = 2 * bx;
            sy = 2 * by;
            best = FLT_MAX;

            for (k = 0; k < n; k++) {
                if (found[k] < best) {
                    best = found[k];
                    sx = shifts[2u * k];
                    sy = shifts[2u * k + 1u];
                }
            }

            bx = sx;
            by = sy;
        }

        /* the cross around the choice (its center the 3 x 3's, the same sums) */
        n = 0;

        for (sy = -1; sy <= 1; sy++) {
            for (sx = -1; sx <= 1; sx++) {
                if (sx == 0 || sy == 0) {
                    shifts[2u * n] = bx + sx;
                    shifts[2u * n + 1u] = by + sy;
                    n++;
                }
            }
        }

        hdrg_st_costs(prev, cur, w, h, shifts, n, found);
        n = 0;

        for (sy = -1; sy <= 1; sy++) {
            for (sx = -1; sx <= 1; sx++) {
                costs[sy + 1][sx + 1] = (sx != 0 && sy != 0) ? 0.0f : found[n++];
            }
        }

        *move_x = (float)bx + hdrg_st_fraction(costs[1][0], costs[1][1], costs[1][2]);
        *move_y = (float)by + hdrg_st_fraction(costs[0][1], costs[1][1], costs[2][1]);
    }

    memcpy(prev, cur, cells * sizeof(float));

    return HDRG_OK;
}

/* the cells of the head's lights take their light's boost */
static void hdrg_st_cells(hdrg_stats_head_t *head)
{
    size_t c, cells;

    cells = (size_t)head->width * head->height;

    for (c = 0; c < cells; c++) {
        if (head->light_of[c] != 0u) {
            head->light[c] = head->lights[head->light_of[c] - 1u].boost;
        }
    }
}

/*
 * The head's lights steadied over its scene's frames before it (their lights
 * as judged: the history's). A future window steadies by place instead
 * (hdrg_st_place), so none is read here
 */
static void hdrg_st_steady_lights(hdrg_st_t *st, hdrg_stats_head_t *head)
{
    const hdrg_stats_light_t *own = st->steady[HDRG_ST_STEADY_LONG];
    const uint32_t move = HDRG_ST_MAX(HDRG_ST_STEADY_MOVE >> HDRG_ST_MIN(head->cell_shift, 3u), 1u),
                   frames = st->steady_frames,
                   past = (st->info.temporal == HDRG_TEMPORAL_WINDOWED)
                              ? HDRG_ST_MIN(st->info.past, frames)
                              : frames;
    uint32_t k, d, n, i, j;
    bool behind;
    float values[HDRG_ST_STEADY_VALUES], v;

    for (k = 0; k < head->light_count; k++) {
        values[0] = own[k].boost;
        n = 1;
        behind = true;

        /* the frames rendered before, back to the cut */
        for (d = 1; d <= frames; d++) {
            behind = behind && d <= past && d <= head->index &&
                     st->steady_index[d - 1u] == head->index - d;
            v = behind
                    ? hdrg_st_vote(st->steady[d - 1u], st->steady_count[d - 1u], &own[k], d * move)
                    : -1.0f;

            if (v >= 0.0f) {
                values[n++] = v;
            }
        }

        /* the median, the lower middle of an even count */
        for (i = 1; i < n; i++) {
            v = values[i];

            for (j = i; j > 0 && values[j - 1u] > v; j--) {
                values[j] = values[j - 1u];
            }

            values[j] = v;
        }

        head->lights[k].boost = values[(n - 1u) / 2u];
    }

    hdrg_st_cells(head);
}

/* a cell's boost as judged in steady_cells' steps (0: no light near) */
static uint8_t hdrg_st_code(const uint32_t *light_of, const float *light, size_t c)
{
    if (light_of == NULL || light == NULL || light_of[c] == 0u) {
        return 0u;
    }

    return (uint8_t)(1u +
                     (uint32_t)(HDRG_ST_CLAMP(light[c], 0.0f, 1.0f) * HDRG_ST_STEADY_STEPS + 0.5f));
}

/* v rounded to the nearest whole, halves away from zero */
static int32_t hdrg_st_round(float v)
{
    return (int32_t)(v + ((v < 0.0f) ? -0.5f : 0.5f));
}

/*
 * OFFLINE: the head's cells' boosts as judged kept aside (the newest of
 * steady_cells), the history's room made on the first frame
 */
static hdrg_status_t hdrg_st_keep(hdrg_st_t *st, const hdrg_stats_head_t *head,
                                  const hdrg_plugin_api_t *api)
{
    const size_t cells = (size_t)head->width * head->height;
    uint32_t d;
    uint8_t *own;
    size_t c;

    /* the session's grid stays: another one is refused (the history is of the first) */
    if (cells == 0 || (st->steady_block != NULL &&
                       (head->width != st->steady_w || head->height != st->steady_h))) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (st->steady_block == NULL) {
        st->steady_block = (uint8_t *)api->mem_calloc(api, (size_t)HDRG_ST_STEADY_LONG + 1u, cells);
        if (st->steady_block == NULL) {
            return HDRG_ERR_NOMEM;
        }

        st->steady_w = head->width;
        st->steady_h = head->height;

        for (d = 0; d <= HDRG_ST_STEADY_LONG; d++) {
            st->steady_cells[d] = st->steady_block + (size_t)d * cells;
        }
    }

    own = st->steady_cells[HDRG_ST_STEADY_LONG];

    for (c = 0; c < cells; c++) {
        own[c] = hdrg_st_code(head->light_of, head->light, c);
    }

    return HDRG_OK;
}

/*
 * PLAYBACK and OFFLINE: each cell of the head's lights takes the mean of the
 * middle half of its place's n boosts in its scene's frames around it (the
 * history's and the queued frames', as judged; a frame without a light there
 * votes 0), the place moved with the camera: a light in a quarter of the
 * frames or fewer takes none, in three quarters or more all, between in steps
 * of 2 / n a frame. A cell the mean leaves at its own step keeps its boost
 * exactly
 */
static void hdrg_st_place(hdrg_st_t *st, hdrg_stats_head_t *head, hdrg_rendering_t *stats,
                          const hdrg_plugin_api_t *api)
{
    const hdrg_stats_frame_t *queued[HDRG_ST_STEADY_LONG];
    const uint32_t w = st->steady_w, h = st->steady_h,
                   reach = st->steady_long ? HDRG_ST_STEADY_LONG
                                           : HDRG_ST_MIN(st->info.past, st->steady_frames);
    const uint8_t *own = st->steady_cells[HDRG_ST_STEADY_LONG];
    uint32_t behind, ahead, d, n, i, j, sum;
    uint8_t values[HDRG_ST_STEADY_VALUES], v;
    int32_t back[HDRG_ST_STEADY_LONG][2], on[HDRG_ST_STEADY_LONG][2], x, y, px, py;
    size_t c;
    float back_x, back_y, on_x, on_y, mean, move_x, move_y;

    /* the frames rendered before, back to the cut, and the queued ones up to the next */
    hdrg_st_move(st, head->index, &move_x, &move_y);
    back_x = 0.0f;
    back_y = 0.0f;
    behind = 0;

    while (behind < reach && behind < head->index &&
           st->steady_index[behind] == head->index - behind - 1u) {
        back_x -= (behind == 0u) ? move_x : st->steady_move[behind - 1u][0];
        back_y -= (behind == 0u) ? move_y : st->steady_move[behind - 1u][1];
        back[behind][0] = hdrg_st_round(back_x);
        back[behind][1] = hdrg_st_round(back_y);
        behind++;
    }

    on_x = 0.0f;
    on_y = 0.0f;
    ahead = 0;

    while (ahead < st->steady_frames &&
           api->stats_queued(stats, ahead + 1u, &queued[ahead]) == HDRG_OK && !queued[ahead]->cut &&
           queued[ahead]->width == w && queued[ahead]->height == h) {
        hdrg_st_move(st, queued[ahead]->index, &move_x, &move_y);
        on_x += move_x;
        on_y += move_y;
        on[ahead][0] = hdrg_st_round(on_x);
        on[ahead][1] = hdrg_st_round(on_y);
        ahead++;
    }

    for (y = 0; y < (int32_t)h; y++) {
        for (x = 0; x < (int32_t)w; x++) {
            c = (size_t)y * w + (size_t)x;

            if (own[c] == 0u) {
                continue;
            }

            values[0] = own[c];
            n = 1;

            for (d = 0; d < behind + ahead; d++) {
                px = x + ((d < behind) ? back[d][0] : on[d - behind][0]);
                py = y + ((d < behind) ? back[d][1] : on[d - behind][1]);

                if (px < 0 || py < 0 || px >= (int32_t)w || py >= (int32_t)h) {
                    continue;
                }

                v = (d < behind)
                        ? st->steady_cells[d][(size_t)py * w + (size_t)px]
                        : hdrg_st_code(queued[d - behind]->light_of, queued[d - behind]->light,
                                       (size_t)py * w + (size_t)px);

                values[n++] = (v != 0u) ? v : 1u;
            }

            for (i = 1; i < n; i++) {
                v = values[i];

                for (j = i; j > 0 && values[j - 1u] > v; j--) {
                    values[j] = values[j - 1u];
                }

                values[j] = v;
            }

            /* the middle half: the mean of the ranks [n / 4, n - n / 4) */
            sum = 0;

            for (i = n / 4u; i < n - n / 4u; i++) {
                sum += values[i];
            }

            mean = (float)sum / (float)(n - 2u * (n / 4u));

            if (mean != (float)own[c]) {
                head->light[c] = (mean - 1.0f) / HDRG_ST_STEADY_STEPS;
            }
        }
    }
}

/*
 * TWO_PASS: the head's lights the first pass judged in the same place take
 * their track's boost; the others stay as judged
 */
static void hdrg_st_track(const hdrg_st_t *st, hdrg_stats_head_t *head)
{
    const hdrg_session_light_t *tracked = &st->tracked[st->tracked_at[head->index]];
    const uint32_t count = st->tracked_at[head->index + 1u] - st->tracked_at[head->index];
    hdrg_session_light_t light;
    uint32_t k;

    for (k = 0; k < head->light_count && k < count; k++) {
        hdrg_st_light(&head->lights[k], head->cell_shift, st->info.width, st->info.height, &light);

        if (light.x0 == tracked[k].x0 && light.y0 == tracked[k].y0 && light.x1 == tracked[k].x1 &&
            light.y1 == tracked[k].y1) {
            head->lights[k].boost = tracked[k].boost;
        }
    }

    hdrg_st_cells(head);
}

/*
 * Before the head's plans: its lights as judged kept aside (the next frames'
 * history), then steadied (TWO_PASS: on the tracks of the first pass)
 */
static hdrg_status_t hdrg_st_steady(hdrg_st_t *st, hdrg_stats_head_t *head, hdrg_rendering_t *stats,
                                    const hdrg_plugin_api_t *api)
{
    const bool filtered =
        st->info.temporal == HDRG_TEMPORAL_RECURSIVE ||
        (st->info.temporal == HDRG_TEMPORAL_WINDOWED && (st->info.past > 0 || st->info.future > 0));
    hdrg_stats_light_t *grown;
    hdrg_status_t status;
    uint32_t d;

    st->steady_count[HDRG_ST_STEADY_LONG] = 0;

    if (st->tracked != NULL && head->lights != NULL) {
        hdrg_st_track(st, head);

        return HDRG_OK;
    }

    if (!filtered) {
        return HDRG_OK;
    }

    if (head->cut) {
        for (d = 0; d < HDRG_ST_STEADY_LONG; d++) {
            st->steady_index[d] = UINT32_MAX;
        }
    }

    if (st->steady_place) {
        status = hdrg_st_keep(st, head, api);

        if (status == HDRG_OK) {
            hdrg_st_place(st, head, stats, api);
        }

        return status;
    }

    if (head->lights == NULL) {
        return HDRG_OK;
    }

    if (st->steady_cap[HDRG_ST_STEADY_LONG] < head->light_count) {
        grown = (hdrg_stats_light_t *)api->mem_alloc(api, (size_t)head->light_count * 2u *
                                                              sizeof(hdrg_stats_light_t));
        if (grown == NULL) {
            return HDRG_ERR_NOMEM;
        }

        api->mem_free(api, st->steady[HDRG_ST_STEADY_LONG]);
        st->steady[HDRG_ST_STEADY_LONG] = grown;
        st->steady_cap[HDRG_ST_STEADY_LONG] = head->light_count * 2u;
    }

    if (head->light_count > 0u) {
        memcpy(st->steady[HDRG_ST_STEADY_LONG], head->lights,
               (size_t)head->light_count * sizeof(hdrg_stats_light_t));
    }

    st->steady_count[HDRG_ST_STEADY_LONG] = head->light_count;
    hdrg_st_steady_lights(st, head);

    return HDRG_OK;
}

/*
 * WINDOWED: the window's mean anchor statistics into the head's: its scene's
 * history and the queued frames up to the next cut. A window of the frame
 * alone leaves them as they are
 */
static void hdrg_st_anchor(const hdrg_st_t *st, hdrg_stats_head_t *head, hdrg_rendering_t *stats,
                           const hdrg_plugin_api_t *api)
{
    const hdrg_stats_frame_t *queued;
    uint32_t i, n;
    float anchor, key;

    anchor = hdrg_math_log2f(HDRG_ST_MAX(head->anchor, 1e-6f));
    key = hdrg_math_log2f(HDRG_ST_MAX(head->key, 1e-6f));
    n = 1;

    for (i = 0; i < st->history_count; i++) {
        anchor += st->history[i].anchor;
        key += st->history[i].key;
        n++;
    }

    for (i = 1;
         i <= st->info.future && api->stats_queued(stats, i, &queued) == HDRG_OK && !queued->cut;
         i++) {
        anchor += hdrg_math_log2f(HDRG_ST_MAX(queued->anchor, 1e-6f));
        key += hdrg_math_log2f(HDRG_ST_MAX(queued->key, 1e-6f));
        n++;
    }

    if (n > 1) {
        head->anchor = hdrg_math_exp2f(anchor / (float)n);
        head->key = hdrg_math_exp2f(key / (float)n);
    }
}

/*
 * The anchor statistics the head's plan uses, per mode: a higher anchor
 * renders dimmer, so RECURSIVE follows a rise at the attack rate
 */
static void hdrg_st_smooth(hdrg_st_t *st, hdrg_stats_head_t *head, hdrg_rendering_t *stats,
                           const hdrg_plugin_api_t *api)
{
    float anchor, key;

    if (st->info.temporal == HDRG_TEMPORAL_WINDOWED) {
        hdrg_st_anchor(st, head, stats, api);

        return;
    }

    if (st->info.temporal == HDRG_TEMPORAL_TWO_PASS) {
        if (st->passes != NULL) {
            head->anchor = st->passes[head->index].anchor_luma;
            head->key = st->passes[head->index].key_luma;
        }

        return;
    }

    anchor = hdrg_math_log2f(HDRG_ST_MAX(head->anchor, 1e-6f));
    key = hdrg_math_log2f(HDRG_ST_MAX(head->key, 1e-6f));

    if (head->cut) {
        st->ema.anchor = hdrg_st_quantize(anchor);
        st->ema.key = hdrg_st_quantize(key);

        return;
    }

    st->ema.anchor = hdrg_st_ema(st->ema.anchor, anchor, st->attack, st->decay);
    st->ema.key = hdrg_st_ema(st->ema.key, key, st->attack, st->decay);
    head->anchor = hdrg_math_exp2f(st->ema.anchor);
    head->key = hdrg_math_exp2f(st->ema.key);
}

/*
 * the light of lights[from, to) whose center is the nearest to light's,
 * within STEADY_MOVE pixels; UINT32_MAX = none
 */
static uint32_t hdrg_st_nearest(const hdrg_session_light_t *lights, uint32_t from, uint32_t to,
                                const hdrg_session_light_t *light)
{
    const int64_t cx = (int64_t)light->x0 + light->x1, cy = (int64_t)light->y0 + light->y1;
    uint32_t k, nearest;
    int64_t dx, dy, best;

    /* centers in half pixels: the reach doubled */
    best = 4 * (int64_t)HDRG_ST_STEADY_MOVE * HDRG_ST_STEADY_MOVE + 1;
    nearest = UINT32_MAX;

    for (k = from; k < to; k++) {
        dx = (int64_t)lights[k].x0 + lights[k].x1 - cx;
        dy = (int64_t)lights[k].y0 + lights[k].y1 - cy;

        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            nearest = k;
        }
    }

    return nearest;
}

static int hdrg_st_compare(const void *a, const void *b)
{
    const hdrg_st_vote_t *x = (const hdrg_st_vote_t *)a, *y = (const hdrg_st_vote_t *)b;

    if (x->track != y->track) {
        return (x->track > y->track) - (x->track < y->track);
    }

    return (x->boost > y->boost) - (x->boost < y->boost);
}

/*
 * TWO_PASS: the first pass's lights, each with the median boost of its track
 * (the lower middle of an even count). A light continues the light of the
 * frame before whose center is the nearest to its own, when its own is that
 * light's nearest too (one light a frame a track); a track ends at a cut
 */
static hdrg_status_t hdrg_st_tracks(hdrg_st_t *st, const hdrg_plugin_api_t *api)
{
    const hdrg_session_pass_t *passes = st->info.passes;
    const hdrg_session_light_t *light;
    hdrg_st_vote_t *votes;
    uint64_t total;
    uint32_t i, k, near, tracks, count, start, end, *track;
    float *medians;

    total = 0;

    for (i = 0; i < st->info.pass_count; i++) {
        if (passes[i].light_count > 0u && passes[i].lights == NULL) {
            return HDRG_ERR_INVALID_ARG;
        }

        for (k = 0; k < passes[i].light_count; k++) {
            light = &passes[i].lights[k];

            if (light->x0 > light->x1 || light->y0 > light->y1 || light->x1 >= st->info.width ||
                light->y1 >= st->info.height || !(light->boost >= 0.0f && light->boost <= 1.0f)) {
                return HDRG_ERR_INVALID_ARG;
            }
        }

        total += passes[i].light_count;
    }

    if (total >= UINT32_MAX || total >= SIZE_MAX / sizeof(hdrg_st_vote_t)) {
        return HDRG_ERR_NOMEM;
    }

    count = (uint32_t)total;
    st->tracked =
        (hdrg_session_light_t *)api->mem_alloc(api, ((size_t)count + 1u) * sizeof(*st->tracked));
    st->tracked_at = (uint32_t *)api->mem_alloc(api, ((size_t)st->info.pass_count + 1u) *
                                                         sizeof(*st->tracked_at));
    track = (uint32_t *)api->mem_alloc(api, ((size_t)count + 1u) * sizeof(*track));
    votes = (hdrg_st_vote_t *)api->mem_alloc(api, ((size_t)count + 1u) * sizeof(*votes));
    medians = (float *)api->mem_alloc(api, ((size_t)count + 1u) * sizeof(*medians));
    if (st->tracked == NULL || st->tracked_at == NULL || track == NULL || votes == NULL ||
        medians == NULL) {
        api->mem_free(api, track);
        api->mem_free(api, votes);
        api->mem_free(api, medians);

        return HDRG_ERR_NOMEM;
    }

    st->tracked_at[0] = 0;

    for (i = 0; i < st->info.pass_count; i++) {
        if (passes[i].light_count > 0u) {
            memcpy(&st->tracked[st->tracked_at[i]], passes[i].lights,
                   (size_t)passes[i].light_count * sizeof(*passes[i].lights));
        }

        st->tracked_at[i + 1u] = st->tracked_at[i] + passes[i].light_count;
    }

    tracks = 0;

    for (i = 0; i < st->info.pass_count; i++) {
        for (k = st->tracked_at[i]; k < st->tracked_at[i + 1u]; k++) {
            near = (i == 0 || st->passes[i].scene_cut)
                       ? UINT32_MAX
                       : hdrg_st_nearest(st->tracked, st->tracked_at[i - 1u], st->tracked_at[i],
                                         &st->tracked[k]);

            if (near != UINT32_MAX &&
                hdrg_st_nearest(st->tracked, st->tracked_at[i], st->tracked_at[i + 1u],
                                &st->tracked[near]) == k) {
                track[k] = track[near];
            } else {
                track[k] = tracks++;
            }

            votes[k].track = track[k];
            votes[k].boost = st->tracked[k].boost;
        }
    }

    qsort(votes, count, sizeof(*votes), hdrg_st_compare);

    for (start = 0; start < count; start = end) {
        end = start + 1u;

        while (end < count && votes[end].track == votes[start].track) {
            end++;
        }

        medians[votes[start].track] = votes[start + (end - start - 1u) / 2u].boost;
    }

    for (k = 0; k < count; k++) {
        st->tracked[k].boost = medians[track[k]];
    }

    api->mem_free(api, track);
    api->mem_free(api, votes);
    api->mem_free(api, medians);

    return HDRG_OK;
}

/*
 * TWO_PASS, second pass: each frame's darkening run and dimming replayed from
 * the first pass's statistics, as the first pass made them (from wherever the
 * second pass starts)
 */
static hdrg_status_t hdrg_st_fades(hdrg_st_t *st, const hdrg_plugin_api_t *api)
{
    const hdrg_session_pass_t *passes = st->info.passes;
    uint32_t i, run;
    float anchor, key, last_anchor, last_key, base;

    st->fades =
        (hdrg_st_fade_t *)api->mem_alloc(api, (size_t)st->info.pass_count * sizeof(*st->fades));
    if (st->fades == NULL) {
        return HDRG_ERR_NOMEM;
    }

    run = 0;
    last_anchor = 0.0f;
    last_key = 0.0f;

    for (i = 0; i < st->info.pass_count; i++) {
        anchor = hdrg_math_log2f(HDRG_ST_MAX(passes[i].anchor_luma, HDRG_ST_KEY_FLOOR));
        key = hdrg_math_log2f(HDRG_ST_MAX(passes[i].key_luma, HDRG_ST_KEY_FLOOR));
        run = (i > 0 && !passes[i].scene_cut &&
               hdrg_st_darkening(key - last_key, anchor - last_anchor))
                  ? HDRG_ST_MIN(run + 1u, st->fade_reach)
                  : 0u;
        last_anchor = anchor;
        last_key = key;
        base = passes[i - run].anchor_luma;

        st->fades[i].run = run;
        st->fades[i].dim =
            (run > 0u && base > 0.0f) ? HDRG_ST_MIN(passes[i].anchor_luma / base, 1.0f) : 1.0f;
    }

    return HDRG_OK;
}

/*
 * TWO_PASS: each frame of the first pass gets its fade, its scene's
 * log-domain mean statistics over the frames outside a darkening run (a
 * fade-out's frames would pull the scene down: they render as the scene,
 * dimmed), each light its track's judgment
 */
static hdrg_status_t hdrg_st_passes(hdrg_st_t *st, const hdrg_plugin_api_t *api)
{
    const hdrg_session_pass_t *passes = st->info.passes;
    hdrg_status_t status;
    uint32_t start, end, i, n;
    double anchor, key, fit;

    for (i = 0; i < st->info.pass_count; i++) {
        if (!(passes[i].apl_fit >= 0.0f && passes[i].apl_fit <= 1.0f)) {
            return HDRG_ERR_INVALID_ARG;
        }
    }

    st->passes =
        (hdrg_session_pass_t *)api->mem_calloc(api, st->info.pass_count, sizeof(*st->passes));
    if (st->passes == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = hdrg_st_fades(st, api);
    if (status != HDRG_OK) {
        return status;
    }

    /* a scene's first frame is in no run: n > 0 */
    for (start = 0; start < st->info.pass_count; start = end) {
        anchor = 0.0;
        key = 0.0;
        fit = 0.0;
        n = 0;

        for (end = start; end < st->info.pass_count && (end == start || !passes[end].scene_cut);
             end++) {
            if (st->fades[end].run > 0u) {
                continue;
            }

            anchor += (double)hdrg_math_log2f(HDRG_ST_MAX(passes[end].anchor_luma, 1e-6f));
            key += (double)hdrg_math_log2f(HDRG_ST_MAX(passes[end].key_luma, 1e-6f));
            fit += (double)hdrg_math_log2f(HDRG_ST_MAX(passes[end].apl_fit, 1e-6f));
            n++;
        }

        for (i = start; i < end; i++) {
            st->passes[i].anchor_luma = hdrg_math_exp2f((float)(anchor / (double)n));
            st->passes[i].key_luma = hdrg_math_exp2f((float)(key / (double)n));
            st->passes[i].apl_fit = hdrg_math_exp2f((float)(fit / (double)n));
            st->passes[i].scene_cut = (i == start);
        }
    }

    return hdrg_st_tracks(st, api);
}

static void HDRG_PLUGIN_CALL hdrg_st_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_st_t *st = (hdrg_st_t *)inst;
    uint32_t d;

    if (st == NULL) {
        return;
    }

    for (d = 0; d <= HDRG_ST_STEADY_LONG; d++) {
        api->mem_free(api, st->steady[d]);
    }

    api->mem_free(api, st->steady_block);
    api->mem_free(api, st->motion_prev);
    api->mem_free(api, st->passes);
    api->mem_free(api, st->tracked);
    api->mem_free(api, st->tracked_at);
    api->mem_free(api, st->fades);
    api->mem_free(api, st);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_st_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const hdrg_stats_session_t *info;
    hdrg_status_t status;
    hdrg_st_t *st;
    uint32_t i;

    (void)options;

    *inst = NULL;

    status = api->filter_stats_session(filter, &info);
    if (status != HDRG_OK) {
        return status;
    }

    st = (hdrg_st_t *)api->mem_calloc(api, 1, sizeof(*st));
    if (st == NULL) {
        return HDRG_ERR_NOMEM;
    }

    st->info = *info;

    for (i = 0; i < HDRG_ST_STEADY_LONG; i++) {
        st->steady_index[i] = UINT32_MAX;
    }

    for (i = 0; i < HDRG_ST_MOVES; i++) {
        st->move_index[i] = UINT32_MAX;
    }

    st->steady_place = info->latency != HDRG_LATENCY_LOW &&
                       info->temporal == HDRG_TEMPORAL_WINDOWED &&
                       (info->past > 0 || info->future > 0);
    st->steady_long = st->steady_place && info->latency == HDRG_LATENCY_OFFLINE;
    st->steady_frames = st->steady_long ? HDRG_ST_STEADY_LONG : HDRG_ST_STEADY_FRAMES;
    st->fade_reach = (uint32_t)HDRG_ST_CLAMP(info->rate * HDRG_ST_FADE_REACH + 0.5f,
                                             (float)HDRG_ST_FADE_STEPS, (float)HDRG_ST_FADE_MAX);

    /* RECURSIVE: the EMA's step a frame, 1 - e^(-frame / tau) */
    st->attack = 1.0f - hdrg_math_expf(-1.0f / (info->rate * HDRG_ST_RECURSIVE_ATTACK));
    st->decay = 1.0f - hdrg_math_expf(-1.0f / (info->rate * HDRG_ST_RECURSIVE_DECAY));

    /* TWO_PASS, second pass: the first pass's scenes, fades and tracks */
    status = (info->temporal == HDRG_TEMPORAL_TWO_PASS && info->passes != NULL)
                 ? hdrg_st_passes(st, api)
                 : HDRG_OK;
    if (status != HDRG_OK) {
        hdrg_st_destroy(st, api);

        return status;
    }

    *inst = st;

    return HDRG_OK;
}

/* the frames a frame's state reads before it (the preroll) and after it, per latency profile */
static void HDRG_PLUGIN_CALL hdrg_st_window(void *inst, uint32_t latency, uint32_t *past,
                                            uint32_t *future, const hdrg_plugin_api_t *api)
{
    const hdrg_st_t *st = (const hdrg_st_t *)inst;
    const hdrg_stats_session_t *info = &st->info;
    const uint32_t reach = info->scene_reach;
    uint32_t frames;
    bool place, longer;

    (void)api;

    place = latency != HDRG_LATENCY_LOW && info->temporal == HDRG_TEMPORAL_WINDOWED &&
            (info->past > 0 || info->future > 0);
    longer = place && latency == HDRG_LATENCY_OFFLINE;
    frames = longer ? HDRG_ST_STEADY_LONG : HDRG_ST_STEADY_FRAMES;

    switch (info->temporal) {
    case HDRG_TEMPORAL_WINDOWED:
        *past = HDRG_ST_MAX(reach, 2u * info->past + reach - 1u + st->fade_reach);

        /* the lights' window behind by place (OFFLINE: long), each frame's motion from the one
           before */
        if (place) {
            *past = HDRG_ST_MAX(*past, frames + 1u + reach + st->fade_reach);
        }

        break;
    case HDRG_TEMPORAL_RECURSIVE:
        /* approximately: the EMA settles, its state never exactly */
        *past =
            (uint32_t)(info->rate * HDRG_ST_RECURSIVE_SETTLE * HDRG_ST_RECURSIVE_DECAY + 0.999f) +
            st->fade_reach + reach;

        break;
    default:
        *past = 0;

        break;
    }

    *future = longer ? HDRG_ST_STEADY_LONG : 0u;
}

/*
 * The darkening run up to the sent frame, at most the fade reach. From its
 * third frame (a fade-out) the frames hold the statistics of the frame
 * before the run, so the mapping stays and the fade stays monotonic (the APL
 * budget would otherwise let the gain rise as the frames darken); a run
 * longer than the reach holds the frame the reach back. TWO_PASS holds
 * nothing (its statistics stay over the scene), and its second pass takes
 * the first pass's runs. From its first frame, the analysis runs again with
 * the bright-component threshold scaled by the dimming against that frame,
 * so a region keeps its judgment (a large white would else drop out of the
 * large-area law and jump up, a light fall below the lights' thresholds)
 */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_st_fade(void *inst, const hdrg_stats_sent_t *sent,
                                                   hdrg_stats_fade_t *out,
                                                   const hdrg_plugin_api_t *api)
{
    const float anchor = hdrg_math_log2f(HDRG_ST_MAX(sent->anchor, HDRG_ST_KEY_FLOOR)),
                key = hdrg_math_log2f(HDRG_ST_MAX(sent->key, HDRG_ST_KEY_FLOOR));
    hdrg_st_t *st = (hdrg_st_t *)inst;
    bool step;
    float base;

    (void)api;

    /* a TWO_PASS second pass past the first pass's frames: none */
    if (st->passes != NULL && sent->index >= st->info.pass_count) {
        return HDRG_ERR_INVALID_ARG;
    }

    step = !sent->cut && hdrg_st_darkening(key - st->fade_key, anchor - st->fade_anchor);
    st->fade_run = step ? HDRG_ST_MIN(st->fade_run + 1u, st->fade_reach) : 0u;
    st->fade_key = key;
    st->fade_anchor = anchor;
    st->p995s[sent->index % HDRG_ST_FADE_SLOTS] = sent->anchor;

    out->run = (st->fades != NULL) ? st->fades[sent->index].run : st->fade_run;
    out->hold =
        (out->run >= HDRG_ST_FADE_STEPS && st->info.temporal != HDRG_TEMPORAL_TWO_PASS) ? 1u : 0u;
    out->from = sent->index - out->run;
    out->cut =
        (uint8_t)((st->passes != NULL) ? (st->passes[sent->index].scene_cut ? 1u : 0u) : sent->cut);
    out->dim = 1.0f;

    if (out->run > 0) {
        base = st->p995s[out->from % HDRG_ST_FADE_SLOTS];
        out->dim = (st->fades != NULL) ? st->fades[sent->index].dim
                   : (base > 0.0f)     ? HDRG_ST_MIN(sent->anchor / base, 1.0f)
                                       : 1.0f;
    }

    return HDRG_OK;
}

/* the sent frame's analysis, final: its camera motion from the frame before (by place) */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_st_frame(void *inst, const hdrg_stats_frame_t *frame,
                                                    const hdrg_plugin_api_t *api)
{
    const uint32_t slot = frame->index % HDRG_ST_MOVES;
    hdrg_st_t *st = (hdrg_st_t *)inst;
    hdrg_status_t status;
    float move_x, move_y;

    move_x = 0.0f;
    move_y = 0.0f;
    status = st->steady_place ? hdrg_st_motion(st, frame, api, &move_x, &move_y) : HDRG_OK;
    st->move_index[slot] = frame->index;
    st->moves[slot][0] = move_x;
    st->moves[slot][1] = move_y;

    return status;
}

/* the frame rendering: its lights steadied, then its anchor statistics */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_st_render(void *inst, hdrg_rendering_t *stats,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_st_t *st = (hdrg_st_t *)inst;
    hdrg_stats_head_t *head;
    hdrg_status_t status;

    status = api->stats_head(stats, &head);
    if (status != HDRG_OK) {
        return status;
    }

    if (head->cut) {
        st->history_count = 0;
        st->history_next = 0;
    }

    st->head_cut = head->cut != 0u;

    status = hdrg_st_steady(st, head, stats, api);
    if (status == HDRG_OK) {
        hdrg_st_smooth(st, head, stats, api);
    }

    return status;
}

/*
 * The APL factor (log2) the head renders with instead of its own fit (none:
 * its own). A lower factor renders dimmer: RECURSIVE follows a fall at the
 * attack rate
 */
static void HDRG_PLUGIN_CALL hdrg_st_fit(void *inst, uint32_t index, float fit, uint8_t *replace,
                                         float *used, const hdrg_plugin_api_t *api)
{
    hdrg_st_t *st = (hdrg_st_t *)inst;
    uint32_t i;

    (void)api;

    /* the scene's fit, or the frame's own where it limits more */
    if (st->info.temporal == HDRG_TEMPORAL_TWO_PASS) {
        if (st->passes != NULL) {
            *used =
                HDRG_ST_MIN(hdrg_math_log2f(HDRG_ST_MAX(st->passes[index].apl_fit, 1e-6f)), fit);
            *replace = 1u;
        }

        return;
    }

    if (st->info.temporal == HDRG_TEMPORAL_RECURSIVE) {
        if (st->head_cut) {
            st->ema.fit = hdrg_st_quantize(fit);

            return;
        }

        st->ema.fit = hdrg_st_ema(st->ema.fit, fit, st->decay, st->attack);
        *used = st->ema.fit;
        *replace = 1u;

        return;
    }

    if (st->history_count == 0) {
        return;
    }

    *used = fit;
    for (i = 0; i < st->history_count; i++) {
        *used += st->history[i].fit;
    }

    *used /= (float)(st->history_count + 1u);
    *replace = 1u;
}

/*
 * after the head's render: its own statistics into the scene's history, its
 * lights (OFFLINE: its cells) as judged into the lights' history
 */
static void HDRG_PLUGIN_CALL hdrg_st_rendered(void *inst, uint32_t index, float anchor, float key,
                                              float fit, const hdrg_plugin_api_t *api)
{
    hdrg_st_t *st = (hdrg_st_t *)inst;
    hdrg_stats_light_t *oldest;
    uint32_t d, cap;
    uint8_t *oldest_cells;

    (void)api;

    if (st->info.past > 0) {
        st->history[st->history_next].anchor = anchor;
        st->history[st->history_next].key = key;
        st->history[st->history_next].fit = fit;
        st->history_next = (st->history_next + 1u) % st->info.past;
        st->history_count = HDRG_ST_MIN(st->history_count + 1u, st->info.past);
    }

    if (st->steady[HDRG_ST_STEADY_LONG] == NULL && st->steady_block == NULL) {
        return;
    }

    /* the frame's take the newest slot and the oldest's room is reused: nothing copies */
    oldest = st->steady[st->steady_frames - 1u];
    oldest_cells = st->steady_cells[st->steady_frames - 1u];
    cap = st->steady_cap[st->steady_frames - 1u];

    for (d = st->steady_frames - 1u; d > 0; d--) {
        st->steady[d] = st->steady[d - 1u];
        st->steady_cells[d] = st->steady_cells[d - 1u];
        st->steady_count[d] = st->steady_count[d - 1u];
        st->steady_cap[d] = st->steady_cap[d - 1u];
        st->steady_index[d] = st->steady_index[d - 1u];
        st->steady_move[d][0] = st->steady_move[d - 1u][0];
        st->steady_move[d][1] = st->steady_move[d - 1u][1];
    }

    st->steady[0] = st->steady[HDRG_ST_STEADY_LONG];
    st->steady_cells[0] = st->steady_cells[HDRG_ST_STEADY_LONG];
    st->steady_count[0] = st->steady_count[HDRG_ST_STEADY_LONG];
    st->steady_cap[0] = st->steady_cap[HDRG_ST_STEADY_LONG];
    st->steady_index[0] = index;
    hdrg_st_move(st, index, &st->steady_move[0][0], &st->steady_move[0][1]);
    st->steady[HDRG_ST_STEADY_LONG] = oldest;
    st->steady_cells[HDRG_ST_STEADY_LONG] = oldest_cells;
    st->steady_count[HDRG_ST_STEADY_LONG] = 0;
    st->steady_cap[HDRG_ST_STEADY_LONG] = cap;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_stats_init(hdrg_plugin_t *plugin,
                                                             const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_ST_ID), HDRG_LIT("Temporal statistics"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_ST_LICENSE), HDRG_LIT(HDRG_ST_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_ST_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_STATS);
    status = (status == HDRG_OK) ? api->class_set_flags(cls, HDRG_FILTER_DETERMINISTIC) : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, hdrg_st_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, hdrg_st_destroy) : status;
    status = (status == HDRG_OK)
                 ? api->class_set_stats(cls, hdrg_st_window, hdrg_st_fade, hdrg_st_frame,
                                        hdrg_st_render, hdrg_st_fit, hdrg_st_rendered)
                 : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}
