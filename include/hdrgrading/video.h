/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading — video: a session grades a sequence of frames (R'G'B' or
 * planar Y'CbCr) into single-layer PQ, the scene statistics following the
 * frames of each scene; the STATIC mapping can be written as a 3D LUT.
 */
#ifndef HDRGRADING_VIDEO_H
#define HDRGRADING_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu.h"
#include "hdrgrading.h"
#include "metadata.h"
#include "sidedata.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HDRG_SESSION_MAX_WINDOW 64u
#define HDRG_SESSION_AUTO       UINT16_MAX /* a window's default (hdrg_session_config_t.past) */

/* how the scene statistics of a frame follow the frames around it */
#define HDRG_TEMPORAL_WINDOWED  UINT8_C(0) /* the mean over [t - past, t + future] in the scene */
#define HDRG_TEMPORAL_RECURSIVE UINT8_C(1) /* an asymmetric EMA, reset at a cut; no latency */
#define HDRG_TEMPORAL_TWO_PASS  UINT8_C(2) /* constant over each scene of a first pass */
#define HDRG_TEMPORAL_STATIC    UINT8_C(3) /* no statistics: one fixed mapping (lut3d_export) */

/*
 * What a session's frames are for (hdrg_session_config_t.latency): how long
 * a frame's output may wait and whether the session must keep up with the
 * stream's rate (hdrg_session_output_t.late, hdrg_session_latency_report).
 * Any preset goes with any of them.
 * - PLAYBACK (default): a video being watched. The ASYNC filters keep the
 *   frames in flight they declared and the statistics look ahead (future,
 *   by default as far as they look back): delay is not limited. Late when
 *   the calls of the last second took longer a frame than the frame
 *   interval at the rate.
 * - LOW (the low-latency mode: a game, a call, a live view): no frame waits
 *   for others: each job of an ASYNC filter is waited for at once, the
 *   statistics window has no future and effort AUTO is REALTIME. Late when a
 *   frame's calls took longer than the frame interval.
 * - OFFLINE: converting a file nobody watches meanwhile: PLAYBACK's
 *   frames, never late, and effort AUTO is QUALITY.
 * The filters' time windows are the same under all of them (their algorithm
 * reads those frames; hdrg_session_latency_report says how many), except
 * the built-in hdrg.mctf's, which reads no frames after its own under LOW.
 */
#define HDRG_LATENCY_PLAYBACK UINT8_C(0)
#define HDRG_LATENCY_LOW      UINT8_C(1)
#define HDRG_LATENCY_OFFLINE  UINT8_C(2)
#define HDRG_LATENCY_PROFILES 3u

/*
 * hdrg_session_latency_t.advice: what to change when the session is late,
 * from what its calls took the most time with (bits):
 * - PREFILTER: fewer or lighter PREFILTER filters, or their GPU versions
 *   (hdrg.metal.*, hdrg.vulkan.*, hdrg.webgpu.*)
 * - EFFORT: the analysis under QUALITY: effort REALTIME
 * - PROXY: the analysis: a smaller proxy (config.proxy)
 * - SCALE: the SCALE stage: a smaller output or a lighter scaler
 * - THREADS: the context has fewer threads than the CPUs
 * - LATENCY: LOW waits for each job of an ASYNC filter: PLAYBACK keeps them
 *   in flight (frames of delay)
 */
#define HDRG_ADVICE_PREFILTER UINT32_C(0x01)
#define HDRG_ADVICE_EFFORT    UINT32_C(0x02)
#define HDRG_ADVICE_PROXY     UINT32_C(0x04)
#define HDRG_ADVICE_SCALE     UINT32_C(0x08)
#define HDRG_ADVICE_THREADS   UINT32_C(0x10)
#define HDRG_ADVICE_LATENCY   UINT32_C(0x20)

/*
 * A video session:
 * frames of one size and color in, one single-layer PQ frame out per input
 * frame, in order. The sequence state (the color LUT, the thread pool, the
 * apply pass's scratch) is built once. Y'CbCr frames are converted to 16-bit
 * R'G'B' when they are sent (the ingest stage): the session
 * holds its queue's depth + 1 of them (the most of the statistics window's
 * future, OFFLINE's 24 frames of the lights' steadying, the SCALE stage's
 * delay and the INTERP window's) and releases the caller's planes at once.
 *
 * Temporal smoothing: a frame is analyzed when it is sent, which also tests
 * it for a scene cut (the scene histogram and the log-average luminance
 * against the previous frame, with no other such change in the 3 frames
 * before: a strobe stays one scene; a fade, the log-average luminance and
 * p99.5 moving the same way together frame after frame, is compared after
 * moving the histogram by the change, so its steps near black do not cut
 * it). When it is rendered, the scene anchor's
 * statistics (p99.5 and the log-average luminance) and the APL budget's
 * factor follow the frames of its scene:
 * - WINDOWED: the log-domain mean of the anchor statistics over the scene's
 *   frames in [t - past, t + future], of the APL factor over [t - past, t].
 * - RECURSIVE: an EMA in the log domain, faster toward a dimmer rendition
 *   (0.15 s) than a brighter one (0.6 s), its state in 1/1024 stop.
 * - TWO_PASS: the anchor statistics are the log-domain mean over the
 *   scene's frames of the first pass (whose cuts are kept) outside its
 *   fade-outs; the APL factor is the lower of the same mean of the first
 *   pass's fits and the frame's own.
 * - STATIC: no analysis and no local gain: every pixel takes the fixed
 *   mapping of its own code values (src/core/plan.h, a fixed plan), which
 *   hdrg_lut3d_export samples.
 *
 * The lights' judgments follow them too, so a light judged
 * near a threshold does not flip: under WINDOWED with a window, PLAYBACK and
 * OFFLINE steady them place by place with the camera followed (each cell of
 * the frame's lights takes the mean of the middle half of what the scene's
 * frames judged where the translation of their cells that matches best from
 * frame to frame moves it, a frame with no light there voting a boost of 0;
 * over 8 frames before it, at most its past, and its queued future, or
 * under OFFLINE over 24 frames before and after it, whatever the
 * statistics' window); under LOW's WINDOWED and RECURSIVE, each light takes
 * the median of the frame's judgment and those of the scene's frames up to 8
 * before it (a frame votes with its light whose center is the nearest,
 * within 8 pixels a frame, or not at all; the lower middle of an even count:
 * in doubt a light stays dimmer); under TWO_PASS, the median over the
 * light's track through the scene of the first pass (a light continues the
 * light of the frame before whose center is the nearest to its own, within 8
 * pixels, when its own is that light's nearest too).
 *
 * The flash limiter (ITU-R BT.1702-3 Annex 1, Guideline 1; on unless
 * flash_off, not under STATIC): the screen luminance of 16 x 9 regions is
 * predicted from the plan's cells, and a change against the previous frame
 * is harmful in a region when the darker side is below 160 cd/m^2 and they
 * differ by 20 cd/m^2 or more, or above it with a Michelson contrast over
 * 1/17. A frame whose harmful change over more than a quarter of the screen
 * would be the seventh within a second (more than three flashes; the
 * changes counted on the renditions before the limiter) renders with the
 * largest gain scale below its own that keeps the change harmless against
 * the previous frame as shown (without gain if the limiter lowered it), at
 * the least none (the SDR rendition's change: the source's own flashes are
 * not removed, only not amplified). Saturated red is not judged.
 * In a fade-out under WINDOWED with a window or RECURSIVE (the log-average
 * luminance and p99.5 falling together for three frames), the frames hold
 * the anchor statistics and the APL factor of the frame before it (without
 * a hold of its own), so the mapping stays and the fade stays monotonic; a
 * fade longer than 2 seconds holds the frame 2 seconds back. From a fade's
 * first darkening frame (TWO_PASS too, from the first pass's statistics:
 * its scene's already stay) the lights' thresholds, the shoulder and the
 * contrast's pivot follow the dimming against the frame before it, so the
 * output darkens in proportion.
 *
 * Seeking: a frame's output reads the frames from
 * hdrg_session_preroll() before it to its window's future. A session created
 * with start = n - p and preroll = p (p of at least that, or n = p) renders
 * frame n and the frames after it byte for byte as a session started at the
 * stream's first frame, for the same frames, pts and settings (WINDOWED,
 * TWO_PASS, STATIC; RECURSIVE only approximately). A frame server sends
 * frames from n - p and receives from n.
 *
 * PREFILTER and SCALE filters with a time window (filter.h,
 * filter_set_window) read the frames around their own: the session keeps
 * them, delays each frame by the windows' future (in hdrg_session_latency)
 * and counts their past in hdrg_session_preroll. An ASYNC filter declares in
 * its create how many frames it keeps in flight (filter_set_max_inflight):
 * under HDRG_LATENCY_PLAYBACK and OFFLINE they delay each frame by that less
 * one, under HDRG_LATENCY_LOW each job is waited for at once (no delay; the GPU
 * and the CPU take turns). hdrg_session_latency_report gives the delay by
 * its causes, under each profile and in milliseconds. At the end of the
 * stream the frames they hold come out with the receives.
 *
 * Under WINDOWED and RECURSIVE, the first frame of a scene and every frame
 * of a zero window render on their own statistics: they equal the still
 * pipeline's output for the same pixels. The analysis leaves black bars at
 * the edges (letterbox, pillarbox) out of the statistics.
 *
 * The statistics across frames are a STATS filter's (the app's added to the
 * stage, else the built-in hdrg.stats.temporal where the build has it:
 * HDRG_WITH_TEMPORAL): without one every frame renders on its own
 * statistics, and a window, RECURSIVE and TWO_PASS give HDRG_ERR_UNSUPPORTED
 * (STATIC stays).
 *
 * Use: send frames until HDRG_ERR_AGAIN, receive until HDRG_ERR_AGAIN, and
 * so on; send NULL at the end, then receive until HDRG_EOF. A session is
 * used by one thread at a time.
 */
typedef struct hdrg_session hdrg_session_t;

/*
 * A session's frames: their size and color (primaries and transfer as
 * hdrg_raw_image_t's), and for R'G'B' input (hdrg_session_pixfmt_t.layout
 * 0) their bit depth (8 or 16; 16: the planes and rows at 2 bytes), the
 * bytes a row and whether a fourth
 * component (alpha, ignored) follows each pixel. The frames bring their
 * pixels (hdrg_session_frame_t)
 */
typedef struct {
    uint32_t width, height;
    uint32_t bit_depth;
    size_t stride;
    uint8_t primaries;
    uint8_t transfer;
    bool has_alpha;
} hdrg_session_format_t;

typedef struct {
    const uint8_t *planes[3]; /* RGB: planes[0], rows of the format's stride; Y'CbCr: Y, Cb, Cr.
                                 Read until release is called */
    size_t strides[3];        /* Y'CbCr: bytes a row of each plane */
    const hdrg_device_frame_t *device; /* config.device: the frame on the GPU instead (planes
                                          and strides not read), copied at the send; NULL:
                                          the planes in host memory */
    const void *motion;                /* the frame's HDRG_SD_MOTION version 1 (sidedata.h: a
                                          codec's, a game engine's vectors; motion_size bytes,
                                          checked and copied at the send), given to its
                                          PREFILTER filters and INTERP jobs; NULL: none */
    size_t motion_size;
    const void *film_grain; /* the frame's HDRG_SD_FILM_GRAIN version 1 (sidedata.h: the grain
                               its decoder left out; film_grain_size bytes, checked and copied
                               at the send), put back in the output; NULL: none */
    size_t film_grain_size;
    int64_t pts;
    void (*release)(void *opaque); /* NULL = none; called once the pixels are no longer read */
    void *opaque;
} hdrg_session_frame_t;

/*
 * A light a TWO_PASS first pass judged: its core's box in the frame's
 * pixels (inclusive, on the analysis cells) and its boost (0..1, a share of
 * the headroom above the diffuse level)
 */
typedef struct {
    uint32_t x0, y0, x1, y1;
    float boost;
} hdrg_session_light_t;

/* a frame's statistics from a TWO_PASS first pass (its outputs, in order) */
typedef struct {
    const hdrg_session_light_t *lights; /* the output's lights, copied (read at create): the
                                           second pass tracks them over the shot */
    uint32_t light_count;
    float anchor_luma;
    float key_luma;
    float apl_fit; /* the APL factor fitted to the frame alone (0..1) */
    bool scene_cut;
} hdrg_session_pass_t;

/* the pixels of a side */
typedef struct {
    uint8_t layout;     /* 0: interleaved R'G'B'; HDRG_YUV_*: planar Y'CbCr */
    uint8_t matrix;     /* Y'CbCr: HDRG_MATRIX_* */
    uint8_t bits;       /* Y'CbCr: 8..16 (above 8: 16-bit samples); R'G'B' output: 8 or 16 */
    uint8_t chroma_loc; /* Y'CbCr input, 4:2:0 / 4:2:2: HDRG_CHROMA_* (default LEFT); the
                           4:2:0 output is LEFT */
    bool full_range;    /* Y'CbCr: false = video (limited) range */
    bool interleaved;   /* Y'CbCr: Cb and Cr alternating in planes[1] (NV12, P010; planes[2]
                           unused), else a plane each */
    bool msb;           /* Y'CbCr above 8 bits: the bits in the high bits of the 16-bit
                           samples, the low ones 0 (P010), else the low ones */
} hdrg_session_pixfmt_t;

/*
 * The temporal mode and its window (at most HDRG_SESSION_MAX_WINDOW each
 * way), the pixels, the analysis size. Initialize with
 * hdrg_session_config_init()
 */
typedef struct {
    uint32_t struct_size;              /* set to sizeof(hdrg_session_config_t) */
    const hdrg_session_pass_t *passes; /* TWO_PASS: the first pass's frames; NULL = this is the
                                          first pass (receive fills the statistics only) */
    uint32_t pass_count;
    uint32_t start;              /* the stream's frame the first one sent is (0 = its first;
                                    TWO_PASS second pass: the first pass's frame); not under a
                                    TWO_PASS first pass */
    uint32_t preroll;            /* the frames sent first that only build the state: rendered
                                    without the apply pass and not received */
    uint32_t rate_num, rate_den; /* frames a second (0 = 30): RECURSIVE's time constants, the
                                    flash limiter's second, the fade-out's reach, the frame
                                    interval the session must keep up with */
    uint32_t out_rate_num;       /* the output's frames a second (0 = the input's): output frame j
                                    at input time j x rate / out_rate, between two input frames
                                    from the INTERP stage's filter (hdrg_context_add_filter;
                                    default hdrg.interp.mc where the build has it, else
                                    HDRG_ERR_UNSUPPORTED), across a cut the nearer frame; each
                                    on the plan of the input frame nearest it (not analyzed
                                    again). Needs rate_num. With a SCALE stage the input frames
                                    are scaled first (in order, ahead of the outputs: the latency
                                    holds the INTERP window's future and the SCALE's delay) and
                                    the INTERP stage reads them at the output's size (the app's
                                    HDRG_SD_MOTION stays of the input's) */
    uint32_t out_rate_den;
    uint32_t output_width; /* the output's size (0 x 0: the input's; one side 0 keeps the
                              aspect ratio): the SCALE stage, a filter added to it or a
                              built-in scaler (hdrg_params_t.upscaler's when larger on both
                              sides, else Lanczos). The analysis stays at the input's size */
    uint32_t output_height;
    uint16_t past;                /* WINDOWED: frames before a frame its statistics average over.
                                     Default HDRG_SESSION_AUTO: 8 with a STATS filter, else 0 */
    uint16_t future;              /* WINDOWED: frames after it, the latency (others and
                                     LOW: 0). Default HDRG_SESSION_AUTO: the past's, LOW 0
                                     (OFFLINE's queue waits for 24 frames at least: the
                                     lights' steadying) */
    uint8_t temporal;             /* HDRG_TEMPORAL_* */
    uint8_t latency;              /* HDRG_LATENCY_* (default PLAYBACK) */
    hdrg_session_pixfmt_t input;  /* layout 0: the format's RGB (its bit depth, stride, alpha) */
    hdrg_session_pixfmt_t output; /* layout 0: PQ R'G'B' (16 bits: codes x 65535); HDRG_YUV_420:
                                     PQ Y'CbCr of the BT.2020 matrix (the BT.2020 container) */
    uint32_t device;              /* HDRG_DEVICE_METAL / VULKAN / WEBGPU (gpu.h): frames may be sent
                                     and received on the GPU (hdrg_session_frame_t.device,
                                     hdrg_session_output_t.device; 0: host memory alone). Needs the
                                     context's HDRG_GPU_AUTO, Y'CbCr input and the frames kept on the GPU
                                     (hdrg_session_latency_t.frames_on_gpu; else HDRG_ERR_UNSUPPORTED), and
                                     for HDRG_FRAME_BUFFERS the context's device of the kind the app's. The
                                     calls wait for their GPU work: an input's writes are done (or before
                                     the send on the device's queue), an output is written when the receive
                                     returns */
    uint16_t proxy;  /* longest side of the analysis image: larger frames are analyzed on a
                        reduction by a power of two (0 = at full resolution; default 960) */
    bool flash_off;  /* no flash limiter */
    bool shed;       /* PLAYBACK / LOW: once the session is late, the frames sent after pass
                        the PREFILTER filters without a time window (the built-in ones among
                        them) through, so it keeps up; the output then follows the timing
                        (hdrg_session_output_t.shed, hdrg_session_latency_t.shed_at) */
    bool dither_off; /* 8-bit R'G'B' output without the triangular dither */
} hdrg_session_config_t;

typedef struct {
    uint8_t *planes[3]; /* the caller's, in the rendition's container primaries: R'G'B' in
                           planes[0] (width * 3 samples a row); Y'CbCr 4:2:0: Y, Cb, Cr */
    size_t strides[3];  /* bytes: R'G'B' at least width * 3 samples */
    const hdrg_device_frame_t *device;  /* config.device: the frame on the GPU instead (NULL:
                                           the planes in host memory) */
    const hdrg_session_light_t *lights; /* TWO_PASS first pass: the lights the frame's analysis
                                           judged, until the next receive (session-owned) */
    uint32_t light_count;
    int64_t pts;        /* the input frame's */
    float peak_nits;    /* the rendition ceiling (hdrg_session_peak; the shoulder saturates
                           toward it) */
    float apl_nits;     /* estimated average luminance of the output */
    float sdr_apl_nits; /* average luminance of the input at SDR white */
    float apl_scale;    /* the APL budget's common gain factor */
    float anchor_luma;  /* the scene's bright anchor (p99.5 of the analysis luminance) */
    float key_luma;     /* the log-average analysis luminance */
    float anchor_used;  /* the p99.5 the plan used (averaged over the window) */
    float apl_fit;      /* the APL factor fitted to this frame alone (apl_scale: averaged) */
    float max_nits;     /* measured: the largest max(R, G, B) of the output (MaxCLL) */
    float avg_nits;     /* measured: the mean max(R, G, B) of the output (FALL) */
    float noise_sigma;  /* the input's Y' noise in 8-bit codes (< 0 = not measured, STATIC) */
    float blockiness;   /* the input's 8-pixel block steps over the others (1 = none) */
    float ringing;      /* the input's ringing around edges, as hdrg_stats_t (1 = none) */
    bool scene_cut;     /* the frame starts a scene */
    bool flash_limited; /* the flash limiter lowered the frame's gain */
    bool fading;        /* in a fade-out: the frame before it's statistics and APL factor */
    bool late;          /* the session does not keep up with the rate (HDRG_LATENCY_*; never
                           OFFLINE): hdrg_session_latency_report says what to change */
    bool shed;          /* rendered without its PREFILTER filters: the session shed them
                           (hdrg_session_config_t.shed) */
    bool synthetic;     /* out_rate: a frame between two input frames (its pts interpolated,
                           the rest the nearer frame's), not an input frame */
    uint32_t phase;     /* ... its place after the input frame of src_pts[0] toward the one of
                           src_pts[1], in 1/65536 (0: an input frame) */
    int64_t src_pts[2]; /* ... the input frames it lies between (an input frame: its pts twice) */
    const void *motion; /* the frame's HDRG_SD_MOTION (sidedata.h): the app's fields, then its
                           PREFILTER filters' (hdrg.mctf's: the motion to the frames of its
                           window); a frame between: its INTERP filter's. Until the next
                           receive (session-owned); NULL: none */
    size_t motion_size;
    hdrg_hdr10plus_t hdr10plus; /* the output's HDR10+ statistics */
    hdrg_st2094_50_t st2094_50; /* ... and its ST 2094-50 gain curve */
} hdrg_session_output_t;

/*
 * A session's delay (hdrg_session_latency_report): the frames a frame's
 * output waits for after it is sent, by their cause, and at the stream's
 * rate; the time the calls took so far. frames = prefilter +
 * prefilter_flight + the most of window, scale + scale_flight and, under
 * out_rate, the INTERP window's future with its jobs ahead in flight (plus
 * scale + scale_flight when there is a SCALE stage too): the session's
 * window and the SCALE stage wait for the same frames.
 */
typedef struct {
    uint32_t struct_size;      /* set to sizeof(hdrg_session_latency_t) */
    uint32_t frames;           /* hdrg_session_latency */
    uint32_t window;           /* the statistics window's future (WINDOWED), or OFFLINE's 24
                                  frames of the lights' steadying when more */
    uint32_t prefilter;        /* the PREFILTER filters' time windows' future */
    uint32_t prefilter_flight; /* the PREFILTER ASYNC filters' frames in flight past the first */
    uint32_t scale;            /* the SCALE filter's time window's future */
    uint32_t scale_flight;     /* its frames in flight past the first */
    uint32_t profiles[HDRG_LATENCY_PROFILES]; /* frames under each HDRG_LATENCY_*, the same filters
                                                 and settings (LOW: no window future); the
                                                 session's own profile's: frames */
    uint32_t preroll;                         /* hdrg_session_preroll */
    uint32_t sent, received;                  /* the frames the measurement covers */
    float rate;                               /* frames a second (the config's, 30 when unset) */
    float frames_ms;   /* frames at the rate: the wait for the frames after it in a live stream */
    float work_ms;     /* measured: the time in send and receive a frame sent (0 before one) */
    float work_max_ms; /* measured: the longest send or receive */
    float total_ms; /* estimated: frames_ms + work_ms, from a live frame's arrival to its output */
    float interval_ms; /* the frame interval at the rate */
    /* --- the parts of work_ms --- measured, a frame sent */
    float convert_ms;        /* Y'CbCr input to R'G'B' */
    float prefilter_ms;      /* the PREFILTER chain (its GPU jobs: the waits for them) */
    float analyze_ms;        /* the analysis and the plan of the grading */
    float apply_ms;          /* the apply pass, the SCALE stage, the output's conversion */
    float interp_ms;         /* out_rate: the frames between (the INTERP stage's filter) */
    uint32_t late_frames;    /* the frames received late (hdrg_session_output_t.late) */
    uint32_t advice;         /* HDRG_ADVICE_* for the part that took the most (0: none to give) */
    uint32_t shed_at;        /* the first frame sent without its PREFILTER filters (UINT32_MAX:
                                none; hdrg_session_config_t.shed) */
    uint32_t convert_device; /* the HDRG_DEVICE_* the Y'CbCr input's conversion runs on (the
                                context's HDRG_GPU_AUTO; 0: the CPU) */
    uint32_t apply_device;   /* ... the apply pass */
    uint8_t profile;         /* the session's HDRG_LATENCY_* */
    bool late;               /* the latest frame received was late */
    bool frames_on_gpu;      /* the frames stay on the GPU from the conversion to the apply
                                (HDRG_GPU_AUTO, Y'CbCr input, the PREFILTER filters on that
                                GPU, no SCALE or out_rate; by default WebGPU's always, Metal's
                                and Vulkan's under LOW alone): the analysis reads their proxy
                                and noise windows read back */
} hdrg_session_latency_t;

/* the defaults; NULL is a no-op */
HDRG_API void hdrg_session_config_init(hdrg_session_config_t *config);
/*
 * config's defaults that depend on the context, resolved as hdrg_session_create
 * resolves them: the HDRG_SESSION_AUTO windows (past 8 where a STATS filter
 * is there, the app's added to the stage or the built-in one, else 0; future
 * the past's for WINDOWED, else 0, LOW's 0). For what a session will be before
 * it is made (a pipeline's latency); NULL is a no-op
 */
HDRG_API void hdrg_session_config_resolve(const hdrg_context_t *ctx, hdrg_session_config_t *config);
/*
 * One setting of a session (hdrg_setting_at's part HDRG_SETTING_SESSION: hdrgrading.h) into
 * config; the errors of hdrg_setting_set
 */
HDRG_API hdrg_status_t hdrg_session_setting_set(const char *name, double value,
                                                hdrg_session_config_t *config);
/* ... and its value from config */
HDRG_API hdrg_status_t hdrg_session_setting_get(const char *name,
                                                const hdrg_session_config_t *config, double *value);
/*
 * format: the frames' size and color, and for RGB input their bit depth,
 * stride and alpha. The output is single-layer PQ
 * at the config's output size. An ASYNC filter that did not declare its
 * max_inflight fails with HDRG_ERR_PLUGIN. caps NULL =
 * hdrg_display_caps_init's; a gamut of HDRG_GAMUT_BT2020 writes a BT.2020
 * container (the content stays within Display P3), which Y'CbCr output
 * needs. params NULL = the defaults; config NULL =
 * hdrg_session_config_init's (WINDOWED with 8 frames past and as many ahead
 * where a STATS filter is there, else none; PLAYBACK, RGB in, 8-bit RGB out,
 * analysis within 960 pixels).
 */
HDRG_API hdrg_status_t hdrg_session_create(hdrg_context_t *ctx, const hdrg_session_format_t *format,
                                           const hdrg_params_t *params,
                                           const hdrg_display_caps_t *caps,
                                           const hdrg_session_config_t *config,
                                           hdrg_session_t **out);
/*
 * NULL = end of stream. HDRG_ERR_AGAIN: receive first; HDRG_ERR_INVALID_STATE
 * after the end. A frame that fails here (its analysis) is not taken: its
 * release is not called
 */
HDRG_API hdrg_status_t hdrg_session_send_frame(hdrg_session_t *session,
                                               const hdrg_session_frame_t *in);
/* HDRG_ERR_AGAIN: send more first; HDRG_EOF: every frame was received after the end */
HDRG_API hdrg_status_t hdrg_session_receive_frame(hdrg_session_t *session,
                                                  hdrg_session_output_t *out);
/*
 * hdrg_session_receive_frame without the apply pass, for the app's GPU: the
 * frame's statistics into out (its planes not written; max_nits and
 * avg_nits 0, not measured) and the state of the shaders into state
 * (state->struct_size set; valid until the next receive). A session
 * without PREFILTER filters, SCALE, out_rate or config.device, not a
 * TWO_PASS first pass (else HDRG_ERR_UNSUPPORTED); the frame is the one
 * sent with out->pts. The library needs no GPU of its own (any HDRG_GPU_*)
 */
HDRG_API hdrg_status_t hdrg_session_receive_state(hdrg_session_t *session,
                                                  hdrg_session_output_t *out,
                                                  hdrg_apply_state_t *state);

/*
 * frames a frame's output waits for after it is sent: the window's future,
 * the filters' time windows' future and their frames in flight (0 for a
 * NULL session)
 */
HDRG_API uint32_t hdrg_session_latency(const hdrg_session_t *session);
/*
 * the delay by its causes and under each profile, in frames and at the
 * rate, with the time measured so far (out->struct_size set)
 */
HDRG_API hdrg_status_t hdrg_session_latency_report(const hdrg_session_t *session,
                                                   hdrg_session_latency_t *out);
/* frames before a frame its output reads: the preroll a seek needs (see above; NULL: 0) */
HDRG_API uint32_t hdrg_session_preroll(const hdrg_session_t *session);
/* the output's size (output_width x output_height resolved; NULL: 0 x 0); either may be NULL */
HDRG_API void hdrg_session_size(const hdrg_session_t *session, uint32_t *width, uint32_t *height);
/*
 * The rendition ceiling (cd/m^2) of every frame of the session, the
 * shoulder's target, known before the first frame: a stream's mastering
 * display maximum (the content's own brightest is measured:
 * hdrg_session_output_t.max_nits); 0 for a NULL session
 */
HDRG_API float hdrg_session_peak(const hdrg_session_t *session);
/* releases the frames still queued */
HDRG_API void hdrg_session_destroy(hdrg_session_t *session);

#define HDRG_LUT3D_MAX_SIZE 129u

/*
 * The STATIC mapping (a session's HDRG_TEMPORAL_STATIC) as a 3D LUT, for
 * FFmpeg lut3d, OBS or mpv: params (NULL = the defaults) on caps (NULL =
 * hdrg_display_caps_init's), for sources of the H.273 primaries and
 * transfer, sampled at size^3 code values (R fastest, then G, then B; node i
 * is the 16-bit code of i / (size - 1)). rgb receives size^3 * 3 PQ R'G'B'
 * values in [0, 1] in the rendition's container primaries, the session's
 * 16-bit output / 65535 for those codes. size is 2..HDRG_LUT3D_MAX_SIZE.
 * HDRG_ERR_UNSUPPORTED when filters were added to the context's SCALE or
 * PREFILTER stage (the LUT is the color mapping alone).
 */
HDRG_API hdrg_status_t hdrg_lut3d_export(hdrg_context_t *ctx, uint8_t primaries, uint8_t transfer,
                                         const hdrg_params_t *params,
                                         const hdrg_display_caps_t *caps, uint16_t size,
                                         float *rgb);

#ifdef __cplusplus
}
#endif

#endif /* HDRGRADING_VIDEO_H */
