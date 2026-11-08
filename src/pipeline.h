/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PIPELINE_H
#define HDRG_PIPELINE_H

#include <stdint.h>

#include "core/analyze.h"
#include "core/apply.h"
#include "core/color.h"
#include "core/exec.h"
#include "core/image.h"
#include "core/plan.h"
#include "core/render.h"
#include "hdrgrading/video.h"
#include "host/analyzer.h"
#include "host/stage.h"
#include "priv.h"

#define HDRG_PROXY_MAX_SHIFT 6u /* a proxy pixel stands for at most 64 x 64 source pixels */
/* the frames a session's PREFILTER chain may hold (time windows, frames in flight), + 1 */
#define HDRG_PIPELINE_HELD (HDRG_PREFILTER_MAX * (HDRG_LIMIT_WINDOW + HDRG_LIMIT_INFLIGHT) + 1u)

/*
 * A session's statistics on its GPU (hdrg_context_desc_t.gpu_analysis FAST):
 * stats gives pass A's sums of the picture analyzed (the source or its
 * proxy) with the bars, moments pass B's of the one planned
 * (hdrg_plan_moments_fn_t); not HDRG_OK: the frame's error (the picture may
 * be on the GPU alone)
 */
typedef struct {
    hdrg_status_t (*stats)(void *arg, const hdrg_source_t *src, uint8_t cell_shift,
                           hdrg_analyze_given_t *given);
    hdrg_plan_moments_fn_t moments;
    void *arg;
} hdrg_pipeline_gpu_stats_t;

/*
 * A session's frames on the device for the PREFILTER stages with a time
 * window (hdrg_pipeline_prefilter_device): keep copies frame (a frame of
 * src/gpu's on the chain's device) into the session's frame k of
 * hdrg_pipeline_device_frames and gives that one back (NULL: it failed)
 */
typedef struct {
    void *(*keep)(void *arg, const void *frame, uint32_t k);
    void *arg;
} hdrg_pipeline_keep_t;

/*
 * Frames through the core, without any container. The sequence part
 * (hdrg_pipeline_open) holds what does not depend on the frame: the source
 * color and its code tables, the color LUT into the rendition gamut, the
 * apply pass's scratch, the gain-map accumulators. Each frame
 * (hdrg_pipeline_frame) runs analysis (pass A) -> plan (pass B) -> render
 * for the display -> SCALE (when the output size differs or a filter is
 * added), then the caller takes the 8-bit rows of its bands (pass C); the
 * two-layer modes also leave the gain-map sums in apply. Nothing at full
 * resolution is held besides the caller's pixels, but for PREFILTER (filters
 * added to the stage, the grain policy REDUCE or deblocking): each frame
 * first goes through the stage's chain into an RGB16 frame, which the core
 * reads instead.
 */
/*
 * A session's delays: the frames its PREFILTER chain and its SCALE stage
 * delay a frame by (their time windows' future and their ASYNC filters'
 * frames in flight past the first) and the frames before its own they read
 */
typedef struct {
    uint32_t chain, chain_flight, chain_past;
    uint32_t scale, scale_flight, scale_past;
} hdrg_pipeline_delay_t;

/*
 * Where hdrg_pipeline_prefilter puts a frame: dst (packed RGB16 rows), or
 * the chain's last stage's frame itself when it landed it, handed to the
 * held frame slot (hdrg_pipeline_hold; UINT32_MAX: dst always); pixels and
 * stride: where it is
 */
typedef struct {
    uint16_t *dst;
    const uint8_t *pixels;
    size_t stride;
    uint32_t slot;
} hdrg_pipeline_out_t;

typedef struct {
    const hdrg_allocator_t *frame_alloc; /* the analysis, plan and render of each frame
                                            (a session's recycler); the context's by default */
    hdrg_params_t params;                /* the sequence's, copied */
    hdrg_encode_params_t encode;         /* the output form (a session's: single-layer PQ) */
    hdrg_display_caps_t caps;
    hdrg_exec_t exec;
    hdrg_color_t color;     /* of the source code values, after sdr_eotf */
    hdrg_source_t src;      /* the frame the core reads */
    hdrg_source_t input;    /* PREFILTER: the caller's frames (src reads the stage's output) */
    hdrg_source_t scaled;   /* the SCALE stage's current band */
    hdrg_source_t scale_in; /* a session's SCALE keeping frames: its ring's frames (RGB16) */
    hdrg_source_t flying;   /* what a flying PREFILTER is configured with: outlives the submit */
    hdrg_source_t proxy;    /* proxy analysis: the reduced frame, 16-bit code values */
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    hdrg_render_t render;
    hdrg_lut_t lut;
    hdrg_apply_t apply;
    hdrg_stage_t scale;               /* the SCALE stage's instance */
    hdrg_analyzer_t analyzer;         /* the ANALYZE stage's (light_predictor > 0; created: open) */
    hdrg_options_t *analyzer_options; /* the built-in analyzer's (resolved defaults) */
    hdrg_stage_t stages[HDRG_PREFILTER_MAX];           /* the PREFILTER chain's instances */
    hdrg_stage_frame_t told;                           /* what the filters are told of a frame */
    double xy[8];                                      /* the source's primaries and white */
    hdrg_filter_entry_t scaler;                        /* the SCALE stage's filter */
    hdrg_filter_entry_t prefilter[HDRG_PREFILTER_MAX]; /* the PREFILTER stage's chain */
    hdrg_filter_entry_t prefilter_cpu[HDRG_PREFILTER_MAX]; /* a built-in on the GPU: its CPU
                                                              filter, without a device */
    hdrg_pipeline_delay_t delay; /* a session's (chain and scale count their flights) */
    hdrg_noise_t input_noise;    /* PREFILTER: the caller's frame's, before the stage */
    hdrg_noise_t held_noise[HDRG_PIPELINE_HELD]; /* input_noise of the chain's frames, by index */
    float held_side[HDRG_PIPELINE_HELD][HDRG_SD_NOISE_FLOATS]; /* ... as side data */
    int64_t held_pts[HDRG_PIPELINE_HELD];                      /* ... and the frames' pts */
    const uint8_t *in_motion; /* PREFILTER: the caller's frame's HDRG_SD_MOTION (NULL: none) */
    size_t in_motion_size;
    uint8_t *held_motion[HDRG_PIPELINE_HELD]; /* in_motion of the chain's frames, by index */
    size_t held_motion_size[HDRG_PIPELINE_HELD], held_motion_cap[HDRG_PIPELINE_HELD];
    uint8_t *added_motion[HDRG_PIPELINE_HELD]; /* ... the fields their filters gave (rows of
                                                  hdrg_sd_motion_field_t and its blocks) */
    size_t added_motion_size[HDRG_PIPELINE_HELD], added_motion_cap[HDRG_PIPELINE_HELD];
    uint32_t added_fields[HDRG_PIPELINE_HELD];
    uint8_t *chain_motion; /* hdrg_pipeline_chain_motion's */
    size_t chain_motion_cap;
    int64_t frame_pts; /* the frame being bound's (the SCALE filter is told of it) */
    hdrg_options_t *default_options;
    hdrg_options_t *prefilter_options[HDRG_PREFILTER_MAX];     /* the built-ins', from the params */
    hdrg_options_t *prefilter_cpu_options[HDRG_PREFILTER_MAX]; /* ... and their CPU filters' */
    uint16_t *filtered;        /* stills with PREFILTER: the frame src reads */
    uint16_t *chain;           /* PREFILTER of two working filters: the frame between two of them */
    hdrg_stage_whole_t *held;  /* a session's: frames of the chain's last stage, by its slots */
    const uint8_t *probed;     /* the frame whose noise open measured (input_noise) */
    const uint16_t *given;     /* a session's frame kept on the GPU: its proxy read back (tight
                                  rows), read instead of reducing the frame (NULL: none) ... */
    const uint16_t *mosaic;    /* ... the noise grid's windows of it, the analysis's noise ... */
    hdrg_noise_grid_t grid;    /* ... on this grid */
    hdrg_stage_t *device_last; /* hdrg_pipeline_prefilter_device's last working stage ... */
    uint32_t device_before;    /* ... the keep frame of the first window's frame the output came
                                  from (the grain's before; UINT32_MAX: the input itself) */
    const hdrg_pipeline_gpu_stats_t *gpu_stats; /* a session's (NULL: the CPU's sums) */
    const hdrg_pipeline_keep_t *keep; /* a session's frames on the device for the windows */
    hdrg_grain_t grain;               /* RESYNTH: the frame's grain (hdrg_pipeline_grain) ... */
    hdrg_grain_stats_t grain_stats;   /* ... and the grain the chain removed from the frame it
                                         gave last (hdrg_pipeline_prefilter) ... */
    hdrg_noise_grid_t grain_grid;     /* ... on the grid of ... */
    uint16_t *grain_windows; /* ... the windows of the frames into the chain's first time window
                                (into the chain without one), grain_frames of them by index, and
                                of the chain's output (NULL: none yet) ... */
    uint32_t grain_held[HDRG_PIPELINE_HELD]; /* ... the frame each holds (index + 1; 0: none) */
    uint32_t grain_frames;
    const uint8_t *in_film; /* PREFILTER: the caller's frame's HDRG_SD_FILM_GRAIN (checked,
                               HDRG_GRAIN_FILM_BYTES; NULL: none) ... */
    uint8_t *held_film;     /* ... of the chain's frames by index (allocated at the first) */
    bool held_film_set[HDRG_PIPELINE_HELD];
    uint8_t film_cached[HDRG_GRAIN_FILM_BYTES]; /* the parameters last converted ... */
    hdrg_grain_stats_t film_stats;              /* ... into this */
    bool film_ready;
    uint16_t *scaled_band;
    const uint16_t *scaled_given; /* the apply pass's SCALE output, a frame scaled apart (NULL:
                                     the stage's bands) ... */
    size_t scaled_given_stride;   /* ... its rows' bytes */
    uint16_t *proxy_pixels;
    size_t gm_cells;        /* two-layer modes: stored gain-map cells */
    uint32_t width, height; /* output */
    uint32_t chain_pushed;  /* frames into the PREFILTER chain */
    uint32_t held_count;    /* the slots of held */
    uint32_t flight_cap;    /* a session's ASYNC filters: at most this many frames in flight */
    uint32_t gpu_first;     /* the built-in filters' GPU versions of this HDRG_DEVICE_* first */
    uint32_t origin;        /* a session: the stream's index of its first frame */
    uint32_t frame_index;   /* the frame being bound (the SCALE filter is told of it) */
    uint8_t proxy_shift;    /* log2 of the source pixels a proxy pixel stands for; 0 = none */
    uint8_t prefilter_count;
    bool scaling;
    bool prefiltering;
    bool windowed;    /* a session's chain keeping frames (a delay or a past) */
    bool streaming;   /* opened without a frame's pixels: a session */
    uint8_t latency;  /* the session's HDRG_LATENCY_* (a still's: OFFLINE) */
    bool scale_keeps; /* a session's SCALE keeps frames: a time window or frames in flight */
    bool scale_apart; /* a session scales its frames apart (hdrg_pipeline_scale_frame), not the
                         bind: the apply pass reads scaled_given */
    bool ended;       /* a session's input ended: SCALE windows end with it */
    bool bypass;      /* a session shed the PREFILTER stage (late): passed through */
} hdrg_pipeline_t;

/*
 * The sequence of raw's format; color describes its code values (raw's
 * primaries / transfer are not read). raw's pixels, when given, are the
 * first frame's (a still's): with a PREFILTER chain, open probes them, and
 * when every filter would pass the frame through it opens without the stage
 * (the core then reads the caller's pixels; a session gives none and keeps
 * the stage for every frame). hdrg_pipeline_frame gives the pixels to read.
 * gpu: the HDRG_DEVICE_* whose versions of the built-in filters it takes
 * first under the context's GPU (0: Metal's, then Vulkan's, then WebGPU's); latency: a
 * session's HDRG_LATENCY_* (LOW: the built-in hdrg.mctf reads no frames
 * after its own)
 */
extern hdrg_status_t hdrg_pipeline_open(hdrg_context_t *ctx, const hdrg_raw_image_t *raw,
                                        const hdrg_color_t *color, const hdrg_params_t *params,
                                        const hdrg_encode_params_t *encode,
                                        const hdrg_display_caps_t *caps, uint32_t gpu,
                                        uint8_t latency, hdrg_pipeline_t *out);
/*
 * The rows' sample depth (8, or 16: the codes x 65535) and whether pass C
 * measures the output light (single-layer PQ and the hybrid's PQ primary:
 * the largest and the mean max(R, G, B) nits, hdrg_pipeline_light, and its
 * hdrg_lightstat_t). Before the first frame.
 */
/*
 * Proxy analysis (video sessions): each frame is analyzed
 * on the center pixels of 2^shift squares (at most HDRG_PROXY_MAX_SHIFT); the
 * plan and the apply pass stay at the source size. Before the first frame.
 */
extern hdrg_status_t hdrg_pipeline_proxy(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                         uint8_t shift);
extern hdrg_status_t hdrg_pipeline_output(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                          uint8_t depth, bool light);
/*
 * One frame of the sequence's format at pixels (kept by the caller until
 * its rows are done): hdrg_pipeline_analyze, hdrg_pipeline_plan and
 * hdrg_pipeline_bind in one
 */
extern hdrg_status_t hdrg_pipeline_frame(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                         const uint8_t *pixels);
/*
 * PREFILTER: the frame at pixels (the format of hdrg_pipeline_open, rows of
 * stride bytes) through the stage's chain into out (RGB16 at the source
 * size: the frame the core reads), its noise measured first (the side data
 * each filter reads; input_noise). Filters passing the frame through are
 * left out; with none left, the frame as it is (pts: the frame's, told to the
 * filters). *index: the chain's input frame out holds, with input_noise its
 * noise and held_pts[*index % HDRG_PIPELINE_HELD] its pts. A session's chain
 * with time windows keeps frames: HDRG_ERR_AGAIN when none came out (dst may
 * have been written). pixels NULL: the input ended, the next frame it held,
 * HDRG_EOF when it holds none.
 */
extern hdrg_status_t hdrg_pipeline_prefilter(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                             const uint8_t *pixels, size_t stride, int64_t pts,
                                             hdrg_pipeline_out_t *out, uint32_t *index);
/*
 * A session's frames of the PREFILTER chain's output: slots of them (its
 * queue's), which hdrg_pipeline_prefilter hands the last stage's frames to
 * instead of copying them into dst (out->slot)
 */
extern hdrg_status_t hdrg_pipeline_hold(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                        uint32_t slots);
/*
 * Every stage of the PREFILTER chain a filter of the library's GPU plugins
 * on the device of kind (filter_set_device; its device frames those of
 * src/gpu), without a clip: a session's frames kept on the GPU go through
 * it on the device (hdrg_pipeline_prefilter_device; its flights capped at
 * 1, the windows' frames the session's: keep)
 */
extern bool hdrg_pipeline_device_chain(const hdrg_pipeline_t *pipeline, uint32_t kind);
/* the session's frames on the device the chain's time windows take (keep's k below it) */
extern uint32_t hdrg_pipeline_device_frames(const hdrg_pipeline_t *pipeline);
/*
 * A session's frame kept on the GPU (in: a frame of src/gpu's on the
 * chain's device) through the PREFILTER chain on the device, the filters
 * told of input_noise and pts. *out: the frame on the device holding the
 * chain's output of its input frame *index (the last working filter's:
 * hdrg_pipeline_device_release once read; a window's frame, keep's; or in
 * itself), with input_noise and held_pts[*index % HDRG_PIPELINE_HELD] its
 * noise and pts. With time windows the chain keeps frames: HDRG_ERR_AGAIN
 * when none came out. in NULL: the input ended, the next frame it held,
 * HDRG_EOF when it holds none.
 */
extern hdrg_status_t hdrg_pipeline_prefilter_device(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                                    void *in, int64_t pts, void **out,
                                                    uint32_t *index);
extern void hdrg_pipeline_device_release(hdrg_pipeline_t *pipeline);
/*
 * Where the caller may write the next frame of a session's chain (RGB16 at
 * the source size, rows of *stride bytes): the first stage's ring slot, so
 * that hdrg_pipeline_prefilter (given *rgb and *stride) does not copy it.
 * HDRG_ERR_UNSUPPORTED when the first stage keeps no frames (or not RGB16).
 */
extern hdrg_status_t hdrg_pipeline_intake(hdrg_pipeline_t *pipeline, uint16_t **rgb,
                                          size_t *stride);
/*
 * pass A of a frame into out (a session analyzes frames ahead of rendering
 * them), allocated from pipeline->frame_alloc; opts as hdrg_analyze's
 */
extern hdrg_status_t hdrg_pipeline_analyze(hdrg_pipeline_t *pipeline, const uint8_t *pixels,
                                           const hdrg_analyze_opts_t *opts, hdrg_analysis_t *out);
/*
 * The plan and the render of the frame at pixels from its analysis (whose
 * planes are released; its statistics stay in pipeline->analysis), the
 * previous frame's released. The render may be rescaled before the bind.
 */
extern hdrg_status_t hdrg_pipeline_plan(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                        const uint8_t *pixels, hdrg_analysis_t *analysis);
/*
 * The plan the frame at pixels would have from its analysis alone, into
 * out (released with hdrg_plan_free on pipeline->frame_alloc): the
 * pipeline's plan and render are left, the analysis kept
 */
extern hdrg_status_t hdrg_pipeline_grid(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                        const uint8_t *pixels, const hdrg_analysis_t *analysis,
                                        hdrg_plan_t *out);
/*
 * The SCALE stage and the apply pass's binding of the planned frame. A
 * session's SCALE keeping frames takes it from its ring (or lands it, in
 * flight): the frames fed to it in order
 */
extern hdrg_status_t hdrg_pipeline_bind(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline);
/*
 * The bound frame's grain from stats (the grain the PREFILTER stage removed
 * from it, or the one its decoder left out: grain_stats as the chain gave
 * the frame) keyed by key (its place in the stream): the apply pass adds it
 * (none: none measured or given, REDUCE)
 */
extern hdrg_status_t hdrg_pipeline_grain(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                         const hdrg_grain_stats_t *stats, uint32_t key);
/*
 * grain_stats from a frame's HDRG_SD_FILM_GRAIN checked (film; NULL: none,
 * grain_stats none): true when given
 */
extern bool hdrg_pipeline_film_grain(hdrg_pipeline_t *pipeline, const uint8_t *film);
/* the chain's frame index's HDRG_SD_FILM_GRAIN as in_film gave it (NULL: none) */
extern const uint8_t *hdrg_pipeline_held_film(const hdrg_pipeline_t *pipeline, uint32_t index);
/*
 * A session's SCALE keeping frames: the frame at pixels (as the core reads
 * it, a scene cut before it or not: the jobs' shots) into its ring, and
 * what can be submitted in flight; pixels NULL: only the latter (after the
 * input ended). depth: the frames a session queues after the one it renders
 * (sized before the first feed)
 */
extern hdrg_status_t hdrg_pipeline_scale_feed(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                              const uint8_t *pixels, int64_t pts, bool cut);
extern void hdrg_pipeline_scale_depth(hdrg_pipeline_t *pipeline, uint32_t depth);
/*
 * A session's ASYNC filters keep at most most frames in flight (1: each job
 * waited for at once); its delays follow. Before the first frame
 */
extern void hdrg_pipeline_cap_flights(hdrg_pipeline_t *pipeline, uint32_t most);
/* a session's delays were its ASYNC filters to keep at most most frames in flight */
extern void hdrg_pipeline_delay(const hdrg_pipeline_t *pipeline, uint32_t most,
                                hdrg_pipeline_delay_t *out);
/* ... a frame rendered for the state alone: taken or landed without the apply pass */
extern hdrg_status_t hdrg_pipeline_scale_skip(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline);
/*
 * scale_apart: the SCALE output of the session's frame (pixels, rows of
 * stride bytes: the next one in order; frame_index, frame_pts and ended
 * set) into out (packed RGB16 of the output size); out NULL: a SCALE
 * keeping frames takes it (in order) and drops it, another does nothing
 */
extern hdrg_status_t hdrg_pipeline_scale_frame(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                               const uint8_t *pixels, size_t stride, uint16_t *out);
/*
 * STATIC: the fixed plan and its render, for every frame from
 * here; a frame then sets src.pixels and goes straight to hdrg_pipeline_bind
 */
extern void hdrg_pipeline_fixed(hdrg_pipeline_t *pipeline);
/*
 * Output rows [y0, y0 + rows) of the current frame into dst (width * 3
 * samples per row of the output depth): y0 a multiple of
 * HDRG_APPLY_BAND_ROWS, rows at most that
 */
extern hdrg_status_t hdrg_pipeline_rows(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline, uint32_t y0,
                                        uint32_t rows, uint8_t *dst);
/*
 * after the last band of the last frame: releases the band state, the
 * scratch and the color LUT, keeps the gain-map sums and statistics
 */
extern void hdrg_pipeline_end(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline);
/* largest log2 tone-map ratio of a finished two-layer frame (at least 0.05) */
extern float hdrg_pipeline_max_ratio(const hdrg_pipeline_t *pipeline);
/* the measured light of the frame's rows (hdrg_pipeline_output): MaxCLL / FALL style, in nits */
extern void hdrg_pipeline_light(const hdrg_pipeline_t *pipeline, float *max_nits, float *avg_nits);
/* ... and its hdrg_lightstat_t, the chunks' summed (none: zero) */
extern void hdrg_pipeline_light_of(const hdrg_pipeline_t *pipeline, hdrg_lightstat_t *out);
/* the analysis and render statistics of the frame into the context */
extern void hdrg_pipeline_stats(hdrg_context_t *ctx, const hdrg_pipeline_t *pipeline);
extern void hdrg_pipeline_close(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline);

/*
 * The HDRG_SD_MOTION the chain holds for its input frame index (the
 * caller's in_motion at its push; NULL: none), *size its bytes
 */
extern const uint8_t *hdrg_pipeline_held_motion(const hdrg_pipeline_t *pipeline, uint32_t index,
                                                size_t *size);
/*
 * ... with the fields the chain's filters gave it after the caller's
 * (HDRG_SD_MOTION_FIELDS at most, the caller's first): the frame's motion
 * out of the chain, until the next call (NULL: none; *size 0 and NULL
 * when there is no memory for it: *ok false)
 */
extern const uint8_t *hdrg_pipeline_chain_motion(hdrg_pipeline_t *pipeline, uint32_t index,
                                                 size_t *size, bool *ok);

#endif /* HDRG_PIPELINE_H */
