/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_ANALYZE_H
#define HDRG_CORE_ANALYZE_H

#include <stdint.h>

#include "hdrgrading/filter.h"
#include "image.h"
#include "noise.h"

#define HDRG_ANALYZE_BINS  4096u /* pass A's histogram of La: bins over [0, 1] */
#define HDRG_ANALYZE_CELLS 5u    /* pass A's planes of the cells (hdrg_analyze_given_t) */
#define HDRG_QUARTER_SHIFT 2u    /* analysis cells are 4x4 pixels ... */
#define HDRG_FINE_SHIFT    1u    /* ... and 2x2 under QUALITY */
/* scene histogram (the scene-cut test of a session): bins of log2 La over [-SCENE_STOPS, 0] */
#define HDRG_SCENE_BINS  16u
#define HDRG_SCENE_STOPS 10.0f
#define HDRG_KEY_FLOOR   0.001f  /* the key's levels floor here (a black frame's key) */
#define HDRG_BAR_LIGHT   0.0025f /* linear max(R, G, B) a black bar's pixels stay below */

/* a judged light (hdrg_analyze_opts_t.lights): where its core is and its boost */
typedef struct {
    uint32_t x0, y0, x1, y1; /* its core's bounding box, cells of the light planes (inclusive) */
    float boost;
} hdrg_light_t;

/*
 * Content analysis output, from one streaming pass over the source (pass A)
 * plus processing on 4x4-pixel cells.
 *
 * Lights: bright regions (cells whose brightest pixel reaches GROW, holding
 * a cell whose brightest channel reaches SEED) are judged by how their
 * surround falls off: a glow, a bloom or a specular's soft edge falls
 * gradually through the rings of cells around the region; a diffuse
 * surface, text, a UI panel or a flat fill falls at once. A light boosts
 * by up to its judgment (0..1, a share of the headroom above the diffuse
 * level); the cells of the region and of its rings take the light's boost
 * and its ramp of log2 La, from lo (its surround) to hi (its core), so the boost
 * fades into the glow continuously. In doubt a region stays diffuse
 * (boost 0): a light missed renders dimmer, never a surface brighter.
 */
typedef struct {
    hdrg_plane_t light;    /* per cell: the boost of the light it belongs to or is near; NULL
                              data = no light */
    hdrg_plane_t light_lo; /* per cell: log2 La where that light's ramp starts */
    hdrg_plane_t light_hi; /* per cell: log2 La where it ends */
    hdrg_plane_t luma;     /* opts->lights: per cell of the planes, the mean relative luminance Y
                              (a session's motion from frame to frame) */
    uint32_t *light_of;    /* opts->lights: per cell of the planes, 1 + the index of the light
                              whose boost it holds (0: none) ... */
    hdrg_light_t *lights;  /* ... and the judged lights (light_count) */
    float *light_features; /* opts->features: HDRG_LIGHT_FEATURES per light (src/core/predict) */
    uint32_t light_count;
    uint32_t bars[4];   /* letterbox: rows / columns left out of the statistics (top,
                           bottom, left, right; analysis pixels) */
    uint8_t cell_shift; /* log2 of the source pixels a cell spans (a proxy's: more) */
    float p995_luma;
    float key_luma;    /* log-average La (geometric mean, floored): overall exposure */
    float light_ratio; /* share of the frame's cells in lights */
    float scene_hist[HDRG_SCENE_BINS]; /* fraction of the pixels in each bin */
    hdrg_noise_t noise;                /* of the full-resolution source (hdrg_pipeline_analyze) */
} hdrg_analysis_t;

/*
 * Pass A's sums from elsewhere (hdrg_analyze_opts_t.given: the GPU's,
 * src/core/gpu_analyze): the cells' HDRG_ANALYZE_CELLS planes (their mean La,
 * brightest La, brightest channel, mean Y and the chroma of the brightest
 * La's pixel), the histogram of La over the bars' rectangle and the bars
 * (when asked for)
 */
typedef struct {
    const float *cells;
    const uint64_t *hist;
    uint32_t bars[4];
} hdrg_analyze_given_t;

/* how a session analyzes its frames */
typedef struct {
    const hdrg_analyze_given_t *given; /* pass A's sums (NULL: from the source's pixels) */
    float bright_scale; /* a darkening frame's dimming (1 = as is): the thresholds of the
                           lights follow it, so its lights keep their judgment */
    uint8_t cell_shift; /* log2 of the cells' side: HDRG_QUARTER_SHIFT or HDRG_FINE_SHIFT; the
                           lights' rings, reach and small sizes stay the same in pixels */
    bool bars;          /* leave black bars at the edges out of the statistics */
    bool lights;        /* list the judged lights (light_of, lights: a session steadies them) */
    bool colored;       /* judge the colored lights too (hdrg_params_t.colored_lights) */
    bool features;      /* the lights' features into light_features (the predictor's data) */
    float predictor;    /* hdrg_params_t.light_predictor: the lights' boost checked by judge */
    /*
     * the chances of the judged lights (an ANALYZE class's: src/host/analyzer), each 1 unless
     * it says less; NULL: none (predictor unused)
     */
    hdrg_status_t (*judge)(void *user, const hdrg_source_t *src, const hdrg_light_record_t *records,
                           uint32_t count, float *chances);
    void *judge_user;
} hdrg_analyze_opts_t;

/*
 * opts NULL = no bars, the thresholds as they are, 4x4 cells.
 * bars: leave the black bars at the edges (letterbox / pillarbox: whole rows
 * or columns whose every pixel is below HDRG_BAR_LIGHT) out of the
 * statistics (p99.5, key, scene histogram); not when they would
 * leave less than a quarter of the frame (a dark frame)
 */
extern hdrg_status_t hdrg_analyze(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                  const hdrg_source_t *src, const hdrg_analyze_opts_t *opts,
                                  hdrg_analysis_t *out);
extern void hdrg_analysis_free(const hdrg_allocator_t *alloc, hdrg_analysis_t *analysis);
/*
 * the black bars (top, bottom, left, right) of a width x height picture
 * from its rows' and columns' largest max(R, G, B): hdrg_analyze's own
 * (a column's below the bars' rows alike: those rows are dark)
 */
extern void hdrg_analyze_bars(const float *rows, const float *columns, uint32_t width,
                              uint32_t height, uint32_t bars[4]);

/*
 * Two-pass 4-connected component labeling (union-find). labels receives one
 * uint32 per pixel: 0 = background, 1..*count = compacted component ids.
 */
extern hdrg_status_t hdrg_label_components(const hdrg_allocator_t *alloc, const uint8_t *mask,
                                           uint32_t width, uint32_t height, uint32_t *labels,
                                           uint32_t *count);

#endif /* HDRG_CORE_ANALYZE_H */
