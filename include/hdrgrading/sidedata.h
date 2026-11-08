/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading — the side data of a frame: its noise (the analysis's,
 * given to the filters), its motion (an app's or a filter's vectors) and its
 * codec's film grain parameters (an app's, from its decoder). An app gives
 * a session's frames their motion and grain in these formats; the filters
 * read and write them (filter.h)
 */
#ifndef HDRGRADING_SIDEDATA_H
#define HDRGRADING_SIDEDATA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Side data of a frame (nego_side_data). HDRG_SD_NOISE, version
 * HDRG_SD_NOISE_VERSION (1 while the ABI is not frozen): the input frame's
 * noise and compression artifacts as HDRG_SD_NOISE_FLOATS
 * floats, levels of its code values over the full scale (an 8-bit code is
 * 1/255). Y', Cb, Cr apply the source's luminance coefficients to its code
 * values.
 *   [0]       Y' over every level; < 0 = not measured (frame too small)
 *   [1] - [8] Y' of the levels in each eighth of [0, 1]; < 0 = too few
 *             samples
 *   [9], [10] Cb, Cr
 *   [11]      blockiness: Y' steps across the 8-pixel grid over those
 *             inside it, on the quieter areas, edges (steps over 12 8-bit
 *             codes) left out (1 = no block artifacts)
 *   [12]      block step: the mean |Y' step| across the grid minus the one
 *             inside it, on the same areas (0 = no block artifacts)
 *   [13]      ringing: in the grid's blocks with an edge, the mean
 *             |Y' - its 3x3 mean| of the samples away from the edge over
 *             the same in the blocks without one (1 = no ringing)
 *   [14]      ringing level: the first mean minus the second (0 = none)
 */
#define HDRG_SD_NOISE         UINT32_C(1)
#define HDRG_SD_NOISE_VERSION UINT32_C(1)
#define HDRG_SD_NOISE_FLOATS  15u

/*
 * HDRG_SD_MOTION, version HDRG_SD_MOTION_VERSION (1 while the ABI is not
 * frozen): the motion of a frame's blocks, in the
 * host's byte order: a hdrg_sd_motion_t, then its fields, each a
 * hdrg_sd_motion_field_t followed by its grid_w x grid_h
 * hdrg_sd_motion_block_t in rows. A field's vector v of the block at q (the
 * frame's pixels) is the motion from frame t to frame t + ref_offset in
 * quarter pixels: the content at q in frame t is at q + v there. An INTERP
 * job's output (job_set_side_data) carries the motion between its two
 * frames at its phase: ref_offset 1, the output's block at q reading the
 * job's frame at q - phase v and the next one at q + (1 - phase) v (the
 * session caps the gain of the output's blocks of low confidence with
 * it). The app gives a session frame's
 * (hdrg_session_frame_t.motion: a codec's, a game engine's): the frame's
 * PREFILTER filters and its INTERP jobs read it (nego_side_data). A
 * PREFILTER job's output (job_set_side_data) adds its fields to the frame's
 * after the app's (HDRG_SD_MOTION_FIELDS at most, the app's first) for the
 * frame's INTERP jobs and the session's output (not the PREFILTER filters
 * after it): hdrg.mctf's, the motion to the frames of its window at phase 0.
 */
#define HDRG_SD_MOTION         UINT32_C(2)
#define HDRG_SD_MOTION_VERSION UINT32_C(1)
#define HDRG_SD_MOTION_FIELDS  8u /* a frame's fields at most */

/* where a field's vectors come from */
#define HDRG_MOTION_INTERNAL UINT32_C(1) /* the library's estimate (hdrg.interp.mc, hdrg.mctf) */
#define HDRG_MOTION_CODEC    UINT32_C(2) /* a decoder's (hints: not for interpolation) */
#define HDRG_MOTION_ENGINE   UINT32_C(3) /* a game engine's */
#define HDRG_MOTION_FLOW     UINT32_C(4) /* an optical flow's */
#define HDRG_MOTION_HOST     UINT32_C(5) /* the app's own */

/* the signal a field was estimated on */
#define HDRG_MOTION_SIGNAL_LUMA UINT32_C(1) /* the luma */
#define HDRG_MOTION_SIGNAL_RGB  UINT32_C(2)
#define HDRG_MOTION_SIGNAL_NONE UINT32_C(3) /* not from the pixels (a codec's, an engine's) */

/* a block's occlusion */
#define HDRG_MOTION_COVERED   UINT8_C(1) /* frame t's content is hidden in t + ref_offset */
#define HDRG_MOTION_UNCOVERED UINT8_C(2) /* t + ref_offset shows content frame t has not */

typedef struct {
    int16_t mv_x, mv_y; /* quarter pixels of the frame */
    uint16_t cost;      /* the match's mean |difference| of the signal, 16-bit codes (0xffff:
                           unknown) */
    uint8_t confidence; /* 0 (none) to 255 (sure) */
    uint8_t occlusion;  /* HDRG_MOTION_COVERED / UNCOVERED (0: none or unknown) */
} hdrg_sd_motion_block_t;

typedef struct {
    uint32_t source;            /* HDRG_MOTION_* */
    int32_t ref_offset;         /* the vectors' frame: t + ref_offset (not 0) */
    uint32_t block_w, block_h;  /* a block's pixels */
    uint32_t grid_w, grid_h;    /* the blocks across and down (the last ones may be partial) */
    int32_t origin_x, origin_y; /* the first block's top-left in the frame's pixels */
    uint32_t phase;    /* INTERP's output: its place between t and t + 1 (1/65536); else 0 */
    uint32_t signal;   /* HDRG_MOTION_SIGNAL_* */
    uint32_t shift;    /* estimated at the frame's size >> shift */
    uint32_t reserved; /* 0 */
} hdrg_sd_motion_field_t;

typedef struct {
    uint32_t fields;   /* 1 to HDRG_SD_MOTION_FIELDS */
    uint32_t reserved; /* 0 */
} hdrg_sd_motion_t;

/*
 * HDRG_SD_FILM_GRAIN, version HDRG_SD_FILM_GRAIN_VERSION (1 while the ABI
 * is not frozen): the film grain a codec carries as
 * parameters its decoder did not add to the frame (FFmpeg's
 * export_side_data=film_grain), in the host's byte order: a
 * hdrg_sd_film_grain_t, then its kind's parameters. HDRG_FILM_GRAIN_AV1: a
 * hdrg_sd_film_grain_av1_t, AV1's film grain synthesis (AFGS1's model) on
 * the coded Y'CbCr the header describes. The app gives a session frame's
 * (hdrg_session_frame_t.film_grain): the session puts the grain in the
 * output as the source would have shown it in SDR (grain_policy PRESERVE
 * and RESYNTH, in place of the grain its PREFILTER stage removed; REDUCE
 * leaves it out).
 */
#define HDRG_SD_FILM_GRAIN         UINT32_C(3)
#define HDRG_SD_FILM_GRAIN_VERSION UINT32_C(1)

#define HDRG_FILM_GRAIN_AV1 UINT32_C(1) /* AV1's film grain synthesis (AFGS1's model) */

typedef struct {
    uint32_t kind;                         /* HDRG_FILM_GRAIN_* */
    uint32_t bit_depth;                    /* the coded luma's, 8 to 12: the parameters' codes */
    uint32_t full_range;                   /* 1: the coded Y'CbCr's full range; 0: video range */
    uint32_t subsampling_x, subsampling_y; /* the chroma's, 0 or 1 (4:2:0: 1 and 1) */
    uint32_t reserved;                     /* 0 */
} hdrg_sd_film_grain_t;

/*
 * AV1's film_grain_params (the AV1 specification's 5.9.30, 6.8.20); the
 * fields the specification stores with an offset are here without it
 */
typedef struct {
    uint8_t y_points[14][2];     /* the luma's scaling function: 8-bit values rising, scalings */
    uint8_t uv_points[2][10][2]; /* Cb's and Cr's */
    int8_t ar_coeffs_y[24];      /* the luma's auto-regression: 2 lag (lag + 1) of them */
    int8_t ar_coeffs_uv[2][25];  /* Cb's and Cr's: as many, then the luma's term */
    int16_t uv_mult[2];          /* cb_mult - 128, cr_mult - 128 */
    int16_t uv_mult_luma[2];     /* cb_luma_mult - 128, cr_luma_mult - 128 */
    int16_t uv_offset[2];        /* cb_offset - 256, cr_offset - 256 */
    uint8_t num_y_points;        /* 0 to 14 */
    uint8_t num_uv_points[2];    /* 0 to 10 */
    uint8_t chroma_scaling_from_luma; /* 1: Cb and Cr scale by the luma's function */
    uint8_t scaling_shift;            /* 8 to 11 */
    uint8_t ar_coeff_lag;             /* 0 to 3 */
    uint8_t ar_coeff_shift;           /* 6 to 9 */
    uint8_t grain_scale_shift;        /* 0 to 3 */
    uint8_t overlap;                  /* (not read) */
    uint8_t reserved;                 /* 0 */
} hdrg_sd_film_grain_av1_t;

#ifdef __cplusplus
}
#endif

#endif /* HDRGRADING_SIDEDATA_H */
