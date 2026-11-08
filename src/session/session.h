/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_SESSION_SESSION_H
#define HDRG_SESSION_SESSION_H

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../core/dynamic.h"
#include "../core/gpu_analyze.h"
#include "../core/gpu_apply.h"
#include "../core/gpu_convert.h"
#include "../core/gpu_gather.h"
#include "../core/motion.h"
#include "../core/recycle.h"
#include "../core/yuv.h"
#include "hdrgrading/foundation/clock.h"
#include "hdrgrading/foundation/fmath.h"
#include "hdrgrading/foundation/fpenv.h"
#include "hdrgrading/foundation/math.h"
#include "hdrgrading/video.h"
#include "../host/defaults.h"
#include "../host/stats.h"
#include "../pipeline.h"
#if defined(HDRG_WITH_CVPIXELBUFFER)
#include "hdrgrading/gpu/apple.h"
#endif

/*
 * The video session (include/hdrgrading/video.h) inside: its state (struct hdrg_session) and the
 * parts its files share. session.c: open, send, receive, the queue and the rendering; flash.c:
 * the flash limiter; rate.c: the output rate (the INTERP stage, the frames it reads, the gain
 * caps of the frames between); resident.c: the frames kept on the GPU and the app's GPU frames;
 * latency.c: the latency's estimate and report, the late frames; export.c: the shaders and the
 * state to run the apply pass on the app's GPU
 */

/* the default window's past frames where a STATS filter makes the statistics across frames */
#define HDRG_SESSION_PAST  8u
#define HDRG_SESSION_PROXY 960u /* the default longest side of the analysis image */

/* the flash limiter (ITU-R BT.1702-3 Annex 1, Guideline 1) */
#define HDRG_FLASH_REGIONS_X 16u
#define HDRG_FLASH_REGIONS_Y 9u
#define HDRG_FLASH_REGIONS   (HDRG_FLASH_REGIONS_X * HDRG_FLASH_REGIONS_Y)
#define HDRG_FLASH_KNEE      160.0f /* cd/m^2: the darker side's level the test changes at */
#define HDRG_FLASH_DIFF      20.0f  /* cd/m^2: below the knee */
#define HDRG_FLASH_CONTRAST  (1.0f / 17.0f) /* Michelson, at and above it */
#define HDRG_FLASH_AREA      0.25f
#define HDRG_FLASH_CHANGES   6u /* changes allowed within a second: three flashes */
#define HDRG_FLASH_STEPS     10u
#define HDRG_FLASH_MAX_RATE  240u

/*
 * A ramp of the exposure (the scene-cut test's): the log-average luminance
 * moving by at least FADE_STEP a frame and p99.5 with it (within FADE_SPREAD
 * of the move, plus FADE_SLACK)
 */
#define HDRG_FADE_STEP   0.02f /* stops */
#define HDRG_FADE_SPREAD 0.5f
#define HDRG_FADE_SLACK  0.05f /* stops */
/* a fade-out's hold (the STATS stage's runs): the frames a run reaches back to, by index */
#define HDRG_FADE_MAX   (2u * HDRG_FLASH_MAX_RATE)
#define HDRG_FADE_SLOTS (HDRG_FADE_MAX + 1u)
/* the parts of the calls' time (hdrg_session_latency_t) */
#define HDRG_PART_CONVERT   0u
#define HDRG_PART_PREFILTER 1u
#define HDRG_PART_ANALYZE   2u
#define HDRG_PART_APPLY     3u
#define HDRG_PART_INTERP    4u
#define HDRG_PARTS          5u

/* hdrg_session_render's modes */
#define HDRG_SESSION_STATE UINT8_C(0) /* the state alone (a preroll frame) */
#define HDRG_SESSION_BIND  UINT8_C(1) /* ... and the plan bound (out_rate: frames between) */
#define HDRG_SESSION_FULL  UINT8_C(2) /* ... and the frame's own apply pass */

/* out_rate: the frames' HDRG_SD_MOTION kept, by index: the queue's and the frame before */
#define HDRG_SESSION_MOTIONS (HDRG_SESSION_MAX_WINDOW + 2u)
/*
 * out_rate: the input frames the INTERP stage reads kept by index: those
 * before the head (its window's past and the frame before); with SCALE,
 * scaled, the head and its window's future too, and the frames of the jobs
 * submitted ahead
 */
#define HDRG_SESSION_KEPT (2u * HDRG_LIMIT_INTERP + 3u + HDRG_SESSION_MAX_WINDOW)

/* the HDR10+ percentiles' percentages (hdrg_hdr10plus_t) */
#define HDRG_SESSION_PURES 3u /* ... the plans of their own: the frame before, head, next */

/* the rate a session without one is taken at (its STATS stage's time constants) */
#define HDRG_RECURSIVE_RATE 30u

/*
 * Scene cut against the previous frame: the L1 distance of the scene
 * histograms (0..2) or the change of the log-average luminance, with no
 * other such candidate within the HDRG_SCENE_MIN_FRAMES - 1 frames before
 * (a strobe stays one scene). A change continuing a ramp of the exposure
 * (a fade) is measured against the previous histogram moved by it, so a
 * candidate also reads the step before: a frame's cut reads the
 * HDRG_SCENE_REACH frames before it
 */
#define HDRG_SCENE_HIST_CUT   0.6f
#define HDRG_SCENE_KEY_CUT    1.0f /* stops */
#define HDRG_SCENE_MIN_FRAMES 4u
#define HDRG_SCENE_MASK       ((1u << HDRG_SCENE_MIN_FRAMES) - 1u)
#define HDRG_SCENE_REACH      (HDRG_SCENE_MIN_FRAMES + 1u)
#define HDRG_SCENE_BIN_STOPS  (HDRG_SCENE_STOPS / (float)HDRG_SCENE_BINS)

/* a sent frame and its analysis, waiting for its future window */
typedef struct {
    hdrg_session_frame_t frame;
    hdrg_analysis_t analysis;
    hdrg_analysis_t faded;    /* darkening (run > 0): analyzed again, the bright threshold dimmed */
    hdrg_noise_t input_noise; /* the frame's as sent (before PREFILTER) */
    hdrg_grain_stats_t grain; /* RESYNTH: the grain its PREFILTER stage removed */
    const uint8_t *pixels; /* the R'G'B' the pipeline reads: the caller's, the ingest buffer ... */
    size_t stride;         /* ... or the PREFILTER chain's frame, its rows' bytes */
    const uint16_t *given; /* frames on the GPU: the proxy read back (NULL: none, or pixels is
                              the frame read back, analyzed whole) ... */
    uint8_t slot;          /* ... and the frame's slot there (HDRG_GC_SLOTS: none) */
    uint32_t index;        /* in the stream */
    uint32_t run;          /* the darkening frames up to it, at most the fade reach */
    uint32_t fade_from;    /* the frame before them */
    float dim;             /* darkening: its p99.5 over that frame's (at most 1) */
    bool cut;
    bool fading; /* in a fade-out (a temporally filtered mode) */
} hdrg_session_item_t;

/* the statistics of a rendered frame its successors average over (log2) */
typedef struct {
    float anchor, key, fit;
} hdrg_session_stat_t;

/* a rendered frame's anchor statistics and APL factor without a fade-out's hold */
typedef struct {
    uint32_t index; /* UINT32_MAX = none */
    float anchor, key, scale;
} hdrg_session_used_t;

struct hdrg_session {
    hdrg_context_t *ctx;
    hdrg_pipeline_t pipeline;
    hdrg_gpu_core_t *gpu;   /* HDRG_GPU_AUTO: the core's passes on a GPU (NULL: the CPU's) */
    hdrg_recycle_t recycle; /* the frames' analyses, plans and renders */
    hdrg_session_config_t config;
    hdrg_session_item_t queue[HDRG_SESSION_MAX_WINDOW + 1u];
    float last_hist[HDRG_SCENE_BINS];            /* the latest sent frame's */
    hdrg_session_used_t unheld[HDRG_FADE_SLOTS]; /* the rendered frames, by index */
    hdrg_stats_stage_t stats;        /* the STATS stage (a temporal mode; created: in use) ... */
    hdrg_stats_session_t stats_info; /* ... the session it is told of ... */
    hdrg_options_t *stats_options;   /* ... the built-in filter's options (resolved defaults) */
    hdrg_session_light_t *judged;    /* TWO_PASS first pass: the output's lights (judged_cap) */
    uint32_t judged_cap;
    hdrg_yuv_t yuv_in, yuv_out; /* Y'CbCr sides (layout 0: R'G'B') */
    uint16_t *ingest;           /* Y'CbCr input or PREFILTER: future + 1 frames of 16-bit R'G'B' the
                                   pipeline reads */
    uint16_t *scratch;          /* Y'CbCr input with PREFILTER: the frame converted for the stage */
    float *yuv_scratch;         /* Y'CbCr input: the chroma sites of the conversion */
    uint8_t *band; /* output rows of one band: Y'CbCr output, strides other than width * 3 */
    size_t ingest_samples; /* a frame's */
    uint32_t count, ingest_next, sent;
    uint32_t taken; /* frames sent (the chain may hold some: sent counts those queued) */
    uint32_t depth; /* the frames queued after the head it waits for: the future window's,
                       or the SCALE stage's when it keeps more */
    uint32_t cands; /* the latest sent frames' cut candidates, the latest in bit 0 */
    uint32_t flash_window, flash_next;      /* the frames of a second, the ring's next slot */
    int8_t flash_dirs[HDRG_FLASH_MAX_RATE]; /* the last second's frames before the limiter: the
                                               direction of a harmful change over the area */
    float flash_prev[HDRG_FLASH_REGIONS];   /* the latest rendered frame's regions before the
                                               limiter, cd/m^2 */
    float flash_zero[HDRG_FLASH_REGIONS];   /* ... without gain, when the limiter lowered it */
    int8_t ramp;                            /* the latest sent frame's step of a ramp of the
                                               exposure: -1 darker, 1 brighter, 0 none */
    float last_key, last_anchor;            /* the latest sent frame's (log2) */
    bool ended, sent_any, flash_held, flash_limited;
    bool drained;                /* the input ended and the chain holds no more */
    bool gpu_convert, gpu_apply; /* gpu: the Y'CbCr input's conversion, the apply pass */
    bool resident;               /* gpu: the frames stay on it from the conversion to the apply */
    uint16_t *gathered;          /* resident: each queue slot's proxy (or frame) read back ... */
    size_t gathered_samples;
    uint16_t *mosaic;        /* ... the noise windows of the latest frame read back ... */
    uint16_t *grain_windows; /* resident RESYNTH: the windows of a frame before and after its
                                chain (the grain removed) */
    hdrg_noise_grid_t grid;  /* ... on this grid ... */
    bool gridded;            /* ... (false: the frames are too small) */
    uint8_t apply_slot;      /* the frame slot the apply pass reads (HDRG_GC_SLOTS: none) */
    uint8_t stats_slot;      /* resident: the frame slot analyzed or planned now (else none) */
    hdrg_pipeline_keep_t window_keep;      /* resident: the chain's windows' frames on gpu */
    hdrg_pipeline_gpu_stats_t gpu_stats;   /* gpu_analysis FAST: the pipeline's sums on gpu ... */
    hdrg_gpu_analyze_t gpu_work;           /* ... their working buffers */
    const hdrg_device_frame_t *out_device; /* config.device: the output's planes on the GPU */
    hdrg_gpu_apply_params_t export_params; /* hdrg_session_receive_state: the uniform ... */
    hdrg_gpu_convert_params_t export_convert; /* ... the conversion's (Y'CbCr input) ... */
    uint8_t *export_luts;                     /* ... the LUT buffer's bytes (made once) ... */
    uint32_t *export_frame;                   /* ... the frame buffer's */
    size_t export_luts_size, export_frame_cap;
    uint64_t work_ns, work_max_ns; /* the time in send and receive, all and the longest call */
    uint64_t part_ns[HDRG_PARTS];  /* ... in each part of them (reports only) */
    uint64_t costs[HDRG_FLASH_MAX_RATE]; /* the last second's frames received: the time in the
                                            calls since the frame before each (a ring) ... */
    uint64_t cost_sum;                   /* ... their sum ... */
    uint64_t work_mark;                  /* ... and work_ns at the latest of them */
    uint32_t cost_next, cost_count;
    uint32_t late_frames; /* received late */
    uint32_t shed_at;    /* the first frame sent without the PREFILTER filters (UINT32_MAX: none) */
    bool late;           /* the latest frame received was late */
    uint32_t received;   /* frames received */
    float rate;          /* frames a second */
    hdrg_stage_t interp; /* out_rate: the INTERP stage */
    hdrg_filter_entry_t interp_entry; /* ... its filter: the one added, or the built-in default */
    hdrg_options_t *interp_options;   /* ... the default's options */
    hdrg_session_output_t anchor; /* out_rate: the state of the input frame whose plan is bound */
    uint8_t *kept;    /* out_rate: the input frames before the head the INTERP stage reads (the
                         frame before and its window's past): copies, by index */
    uint16_t *scaled; /* out_rate with SCALE: the frames scaled apart (output size, packed RGB16)
                         instead, by index, from the past's to the window's future */
    uint32_t scaled_next; /* ... the next frame to scale (in order) */
    uint32_t first_out;   /* ... the first frame an output is at (start + preroll) */
    uint8_t *synth; /* out_rate, a source of 8 bits or with alpha: a frame in the source's layout */
    size_t row_bytes;        /* out_rate: a row of the source's R'G'B' */
    uint64_t out_next;       /* out_rate: the next output frame, from the stream's first */
    uint64_t rate_p, rate_q; /* out_rate: output frame j at input time j p / q (q 0: no out_rate) */
    int64_t kept_pts[HDRG_SESSION_KEPT];
    uint32_t kept_index[HDRG_SESSION_KEPT]; /* ... their frames (UINT32_MAX: none) ... */
    uint8_t kept_cut[HDRG_SESSION_KEPT];    /* ... and their scene cuts */
    uint32_t kept_frames;                   /* ... of them */
    uint32_t *phases; /* out_rate: the phases of an interval's outputs (phase_cap at most) */
    uint32_t phase_cap;
    uint64_t interp_outs[HDRG_LIMIT_INFLIGHT]; /* out_rate, INTERP in flight: the outputs whose
                                                  jobs are submitted, in order ... */
    uint64_t interp_scan;                      /* ... the next output to consider */
    uint32_t interp_count;                     /* ... of them */
    bool interp_ahead; /* ... the INTERP filter keeps jobs in flight (ASYNC, not LOW) */
    uint32_t bound;    /* out_rate: the input frame whose plan is bound (UINT32_MAX: none) */
    uint8_t *sd_motion[HDRG_SESSION_MOTIONS]; /* out_rate: the frames' HDRG_SD_MOTION as sent
                                                 (a copy), by index ... */
    size_t sd_motion_size[HDRG_SESSION_MOTIONS], sd_motion_cap[HDRG_SESSION_MOTIONS];
    uint32_t sd_motion_index[HDRG_SESSION_MOTIONS]; /* ... their frames (UINT32_MAX: none) */
    hdrg_plan_t pure[HDRG_SESSION_PURES];    /* capping: the frames' plans from their analyses
                                                alone, by index ... */
    uint32_t pure_index[HDRG_SESSION_PURES]; /* ... their frames (UINT32_MAX: none) */
    float *capped;                           /* capping: the bound plan's boost grid capped for a
                                                frame between (capped_cells) */
    size_t capped_cells;
    bool capping; /* out_rate: a frame between's boost capped by the INTERP stage's motion: its
                     filter may give HDRG_SD_MOTION and plans have boost */
};

/* round(v x part / q), the same for either sign */
static inline int64_t hdrg_session_share(int64_t v, uint64_t part, uint64_t q)
{
    const int64_t n = 2 * v * (int64_t)part + (int64_t)q, d = 2 * (int64_t)q;

    return (n >= 0) ? n / d : -((-n + d - 1) / d);
}

/* the time of a send or receive that began at start (reports only: never the output's) */
static inline void hdrg_session_clock(hdrg_session_t *session, uint64_t start)
{
    const uint64_t now = hdrg_clock_ns(), spent = (now > start) ? now - start : 0u;

    session->work_ns += spent;
    session->work_max_ns = HDRG_MAX(session->work_max_ns, spent);
}

/* the time of a part of a call since start (reports only); the time now */
static inline uint64_t hdrg_session_part(hdrg_session_t *session, uint8_t part, uint64_t start)
{
    const uint64_t now = hdrg_clock_ns();

    session->part_ns[part] += (now > start) ? now - start : 0u;

    return now;
}

extern void hdrg_session_release(const hdrg_session_frame_t *frame);

/* frame index's HDRG_SD_MOTION (checked) kept for the INTERP stage and the output (false: no
 * memory) */
extern bool hdrg_session_store_motion(hdrg_session_t *session, uint32_t index, const void *motion,
                                      size_t size);

/* frame index's HDRG_SD_MOTION as queued (NULL: none), *size its bytes */
extern const uint8_t *hdrg_session_motion_of(const hdrg_session_t *session, uint32_t index,
                                             size_t *size);

/* capping: the item's plan from its analysis alone (before any steadying), kept by index */
extern hdrg_status_t hdrg_session_pure(hdrg_session_t *session, const hdrg_session_item_t *item);

/*
 * The flash limiter. The changes are those of the renditions before it (each
 * frame's own APL factor), so a frame's limit reads the last second and no
 * further. When the head item's change would exceed HDRG_FLASH_CHANGES and
 * is a harmful rise (a lower gain does not soften a fall), its gain scale is
 * lowered to the largest one (bisected) whose change is no harmful rise,
 * none at the least. That change is judged against the previous frame as
 * shown: its regions, or without gain when the limiter lowered it (the least
 * it showed, so a rise is not underrated)
 */
extern hdrg_status_t hdrg_session_flash(hdrg_session_t *session, hdrg_session_output_t *out);

/*
 * The bound frame's apply pass over pixels (rows of stride bytes: the head's,
 * or a frame between two of them) into out (Y'CbCr output: through the band
 * into yuv), and its light: the GPU's whole frame, or the CPU's bands (from
 * the frame the GPU fails on)
 */
extern hdrg_status_t hdrg_session_apply(hdrg_session_t *session, const hdrg_yuv_t *yuv,
                                        const uint8_t *pixels, size_t stride,
                                        const hdrg_grain_stats_t *grain, uint32_t key,
                                        hdrg_session_output_t *out);

/*
 * The head item through plan, render (the smoothed APL factor; STATIC: the
 * fixed ones) and the flash limiter into out's fields: under
 * HDRG_SESSION_FULL then the apply pass of its own pixels into out, under
 * HDRG_SESSION_BIND bound for frames between it and its neighbors (out's
 * fields alone). A preroll frame (HDRG_SESSION_STATE) renders for the state
 * of the frames after it alone: nothing bound, out not filled
 */
extern hdrg_status_t hdrg_session_render(hdrg_session_t *session, const hdrg_yuv_t *yuv,
                                         uint8_t mode, hdrg_session_output_t *out);

/* the head item is done: released and dropped */
extern void hdrg_session_pop(hdrg_session_t *session);

/* out_rate: the head item done: kept as the frame before, released and dropped */
extern hdrg_status_t hdrg_session_pass(hdrg_session_t *session);

/*
 * The frame at pixels (the R'G'B' the pipeline reads, rows of stride bytes;
 * frame: the caller's, its pts and release) analyzed onto the queue;
 * input_noise: its noise as it came (NULL: the analysis's)
 */
extern hdrg_status_t hdrg_session_queue(hdrg_session_t *session, const hdrg_session_frame_t *frame,
                                        const uint8_t *pixels, size_t stride,
                                        const hdrg_noise_t *input_noise);

/*
 * resident: a frame of the PREFILTER chain's windows copied into the
 * session's frame k on the GPU after the queue's slots (the pipeline's keep)
 */
extern void *hdrg_session_window_keep(void *arg, const void *frame, uint32_t k);

/* resident: the frames the PREFILTER chain's windows still hold at the end, on the GPU */
extern hdrg_status_t hdrg_session_drain_resident(hdrg_session_t *session);

/*
 * The input ended: the frames the PREFILTER chain's time windows still
 * hold onto the queue, while it has room
 */
extern hdrg_status_t hdrg_session_drain(hdrg_session_t *session);

/*
 * out_rate: the next output frame j, at input time t = j p / q: on the plan
 * of the input frame nearest it (bound once), the input frame itself when t
 * is one, else the frame between the two around t from the INTERP stage (the
 * nearer one across a cut, or without the other). The head waits for the
 * frame after it (depth >= 1); a head behind the outputs is kept as the
 * frame before and dropped.
 */
extern hdrg_status_t hdrg_session_receive_rate(hdrg_session_t *session, const hdrg_yuv_t *yuv,
                                               hdrg_session_output_t *out);

/*
 * The frame just received: the time in the calls since the frame before
 * into the last second's ring. Late under PLAYBACK when their mean is over
 * the frame interval (once the ring holds a second), under LOW when the
 * frame's own is (from the second frame: the first sets up), never under
 * OFFLINE
 */
extern void hdrg_session_late(hdrg_session_t *session, hdrg_session_output_t *out);

/* the pipeline's pass A on the session's GPU (hdrg_pipeline_gpu_stats_t) */
extern hdrg_status_t hdrg_session_gpu_sums(void *arg, const hdrg_source_t *src, uint8_t cell_shift,
                                           hdrg_analyze_given_t *given);

/* ... and its pass B (hdrg_plan_moments_fn_t) */
extern hdrg_status_t hdrg_session_gpu_moments(void *arg, const hdrg_source_t *src,
                                              const hdrg_analysis_t *analysis, uint8_t light_shift,
                                              uint32_t cell, uint32_t grid_w, uint32_t grid_h,
                                              double *sums, float *grid);

/*
 * out_rate: the INTERP stage (the filter added to it, else hdrg.interp.mc:
 * under the context's HDRG_GPU_AUTO / FILTERS its Metal, Vulkan or WebGPU
 * version, the CPU's without a device), the frame kept before the head
 * and, for a source of 8 bits or with alpha, a frame in its layout; the
 * head waits for the frame after it; the outputs from the first at or
 * after the first frame received (start + preroll)
 */
extern hdrg_status_t hdrg_session_rate_open(hdrg_context_t *ctx, hdrg_session_t *session);

/*
 * frames on the GPU: each queue slot's proxy read back (the frame, analyzed
 * whole) and the noise windows of a frame
 */
extern hdrg_status_t hdrg_session_resident_buffers(hdrg_session_t *session);

/*
 * Frames on the GPU: the Y'CbCr frame converted into the next queue slot's
 * frame there, through the PREFILTER chain on the device (told of its noise,
 * the last filter's frame copied back into the slot), its proxy and noise
 * windows (or itself, analyzed whole) read back and analyzed. The input's
 * planes are released once uploaded
 */
extern hdrg_status_t hdrg_session_send_resident(hdrg_session_t *session,
                                                const hdrg_session_frame_t *in,
                                                const hdrg_yuv_t *yuv);

/*
 * A frame on the app's GPU the session takes for a side of pixfmt: its
 * buffers the context's device of the kind the app's (HDRG_FRAME_BUFFERS),
 * its dma-bufs for a Vulkan session (HDRG_FRAME_DMABUF: imported by the
 * plane copies), its pixel buffer's format the side's
 * (HDRG_FRAME_CVPIXELBUFFER: the size the plane copies check), an
 * AHardwareBuffer for a Vulkan session of an NV12 or P010 side
 * (HDRG_FRAME_AHARDWAREBUFFER: its format and size the plane copies check).
 * HDRG_ERR_UNSUPPORTED: a kind not taken
 */
extern hdrg_status_t hdrg_session_device_frame(const hdrg_session_t *session,
                                               const hdrg_device_frame_t *frame,
                                               const hdrg_session_pixfmt_t *pixfmt);

/* the frame bound to its plan (not applied) as the state of the app's apply shader */
extern hdrg_status_t hdrg_session_export(hdrg_session_t *session, const hdrg_yuv_t *yuv,
                                         hdrg_apply_state_t *state);

extern hdrg_status_t hdrg_session_receive(hdrg_session_t *session, hdrg_session_output_t *out,
                                          hdrg_apply_state_t *state);

#endif /* HDRG_SESSION_SESSION_H */
