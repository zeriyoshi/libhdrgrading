/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_APPLY_H
#define HDRG_CORE_APPLY_H

#include <stdint.h>
#include <string.h>

#include "../kernels/kernels.h"
#include "color.h"
#include "exec.h"
#include "grain.h"
#include "image.h"
#include "plan.h"
#include "render.h"

/* rows rendered per band: a multiple of HDRG_EXEC_GRAIN_ROWS */
#define HDRG_APPLY_BAND_ROWS 256u

#define HDRG_LIGHT_BINS 160u  /* hdrg_lightstat_t: 8 an octave of max(R, G, B) cd/m² ... */
#define HDRG_LIGHT_LOW  (-6)  /* ... from 2^-6 (below it: the first) to 2^14 */
#define HDRG_LIGHT_STEP 4u    /* the ratio's samples: every 4th pixel of every 4th row */
#define HDRG_LIGHT_Q    64.0f /* the ratio's log2 in 1/64 stop ... */
#define HDRG_LIGHT_SPAN 512   /* ... within +-8 stops, + 8 stops each (unsigned sums) */

/*
 * A frame's light (single-layer PQ with its MaxCLL / FALL): its largest
 * R, G, B, and on every HDRG_LIGHT_STEP pixel of every
 * HDRG_LIGHT_STEP row the output's max(R, G, B) cd/m² by its bin
 * (hdrg_lightstat_bin) and the log2 of the SDR source's max(R, G, B) (into
 * the rendition gamut, at the SDR white's cd/m²) over it, by that bin: the
 * gain back to the SDR (sums of integers, any order)
 */
typedef struct {
    uint64_t count[HDRG_LIGHT_BINS];   /* (64 bits: a still's sums of 2^28 pixels) */
    uint64_t samples[HDRG_LIGHT_BINS]; /* the ratio's ... */
    uint64_t ratio[HDRG_LIGHT_BINS];   /* ... the sum of its Q6 + HDRG_LIGHT_SPAN ... */
    uint64_t place[HDRG_LIGHT_BINS];   /* ... and of their places in the bin (the 8 mantissa
                                          bits after its 3: 1/256 of it) */
    float channel[3];
} hdrg_lightstat_t;

/*
 * Pass C: renders source rows straight into 8-bit output rows in one fused
 * pass (code values -> linear luminance -> gain from the plan -> grading
 * ratio; code values -> color LUT (gamut mapping and saturation) -> x ratio
 * -> highlight desaturation and shoulder -> OETF -> hash dither -> pack).
 * The math of the stages runs in the SIMD tier's kernels (src/kernels/).
 * The two-layer modes accumulate the per-pixel log2 tone-map ratio
 * into the cells of the stored gain map in the same pass. Bands start on
 * multiples of HDRG_EXEC_GRAIN_ROWS, so each gain-map cell (at most 16 rows)
 * is summed by one chunk in a fixed order: the result depends neither on the
 * band split nor on the worker count.
 */
typedef struct {
    const hdrg_source_t *src;
    const hdrg_plan_t *plan;       /* the boost grid and the clarity base */
    const hdrg_lut_t *lut;         /* source codes -> rendition-gamut linear RGB */
    const hdrg_kernels_t *kernels; /* the SIMD tier's kernels */
    hdrg_grade_args_t grade;
    hdrg_layer_args_t layer;       /* two-layer modes */
    hdrg_shoulder_args_t shoulder; /* single-layer PQ */
    float diffuse, span;           /* the render's gain: diffuse + boost share x span, stops */
    float grid_scale_x;            /* plan cells per output pixel (the output may be scaled) */
    float grid_scale_y;
    float *gm_sum;      /* two-layer modes: log2 ratio sums per stored gain-map cell */
    float *chunk_max;   /* two-layer modes: largest log2 ratio per HDRG_EXEC_GRAIN_ROWS rows */
    float *chunk_cll;   /* single-layer PQ, NULL = off: largest max(R, G, B) nits per chunk ... */
    double *chunk_fall; /* ... and their sum (MaxCLL / MaxFALL) */
    hdrg_lightstat_t *chunk_light; /* ... and their light, NULL = none (the GPU's: the first) ... */
    float light_matrix[9]; /* ... the ratio's SDR: the source's linear RGB into the rendition
                              gamut ... */
    float light_white;     /* ... at its white's cd/m² */
    float *la_rows;        /* per worker: La and max(R, G, B) of the rows above, at and below the
                              output row and their column sums (8 x width), for a cored plan */
    const hdrg_grain_t *grain; /* the frame's grain added to the PQ rows (NULL: none) */
    int32_t *grain_rows; /* per worker: the grain's rows and scratch (hdrg_apply_grain_rows) */
    float *grid_tx;      /* per output column: its weight toward the next plan cell ... */
    uint32_t *grid_x;    /* ... and its cell (hdrg_grid_pos across, set by the bind) */
    uint32_t workers;    /* slots of la_rows: the concurrency of the exec the rows run on */
    uint32_t gm_w;
    uint8_t gm_shift;     /* stored gain-map cells are 2^gm_shift pixels per side (<= 16) */
    uint8_t mode;         /* hdrg_output_mode_t */
    uint8_t sample_bytes; /* 1: 8-bit rows; 2: 16-bit rows (the codes x 65535, no dither) */
    bool banded;          /* src holds only the rows of each hdrg_apply_rows call */
    bool dither;
} hdrg_apply_t;

/*
 * hdrg_lightstat_t's bin of max(R, G, B) cd/m²: its float's exponent and top 3
 * mantissa bits (bin b from 2^(b / 8 + HDRG_LIGHT_LOW) (1 + (b % 8) / 8),
 * linear inside); the GPU's alike (apply.wgsl)
 */
static inline uint32_t hdrg_lightstat_bin(float nits)
{
    uint32_t bits;

    if (!(nits >= 0x1p-6f)) {
        return 0;
    }

    memcpy(&bits, &nits, sizeof(bits));

    return HDRG_MIN((((bits >> 23) & 0xffu) - (uint32_t)(127 + HDRG_LIGHT_LOW)) * 8u +
                        ((bits >> 20) & 7u),
                    HDRG_LIGHT_BINS - 1u);
}

/* ... a value's place in its bin, 0 to 255 (below 2^HDRG_LIGHT_LOW: 0) */
static inline uint32_t hdrg_lightstat_place(float nits)
{
    uint32_t bits;

    if (!(nits >= 0x1p-6f)) {
        return 0;
    }

    memcpy(&bits, &nits, sizeof(bits));

    return (bits >> 12) & 0xffu;
}

/* ... and the least cd/m² of bin b (the first's: 0) */
static inline float hdrg_lightstat_lower(uint32_t b)
{
    uint32_t bits;
    float v;

    bits = ((uint32_t)(127 + HDRG_LIGHT_LOW) + b / 8u) << 23 | (b % 8u) << 20;
    memcpy(&v, &bits, sizeof(v));

    return (b == 0) ? 0.0f : v;
}

/*
 * The sequence part: src has the output size (the source itself, or the
 * SCALE stage's bands: banded, so the coring's 3x3 mean stays inside each
 * call's rows); lut maps its code values into the rendition gamut;
 * params supplies the grading, encode the output mode and the dither.
 * kernels NULL = scalar. exec is the one
 * hdrg_apply_rows runs on (its per-worker grid rows are allocated here).
 * Bind a frame's plan and render before its rows; release with
 * hdrg_apply_free. The gain-map accumulators are the caller's.
 */
extern hdrg_status_t hdrg_apply_setup(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                      const hdrg_source_t *src, bool banded, const hdrg_lut_t *lut,
                                      const hdrg_kernels_t *kernels, const hdrg_params_t *params,
                                      const hdrg_encode_params_t *encode, hdrg_apply_t *out);
/* the frame part: its plan and render (both kept by the caller while its rows run) */
extern void hdrg_apply_bind(hdrg_apply_t *apply, const hdrg_plan_t *plan,
                            const hdrg_render_t *render, const hdrg_params_t *params);
/* each worker's grain rows and their scratch, once (RESYNTH: from the setup) */
extern hdrg_status_t hdrg_apply_grain_rows(const hdrg_allocator_t *alloc, hdrg_apply_t *apply);
extern void hdrg_apply_free(const hdrg_allocator_t *alloc, hdrg_apply_t *apply);

/*
 * Renders source rows [y0, y0 + rows) into dst (interleaved RGB, width * 3
 * bytes per row). y0 must be a multiple of HDRG_EXEC_GRAIN_ROWS.
 */
extern void hdrg_apply_rows(const hdrg_exec_t *exec, const hdrg_apply_t *apply, uint32_t y0,
                            uint32_t rows, uint8_t *dst);

#endif /* HDRG_CORE_APPLY_H */
