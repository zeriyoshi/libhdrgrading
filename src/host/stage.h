/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_STAGE_H
#define HDRG_HOST_STAGE_H

#include <stdint.h>

#include "../core/exec.h"
#include "../core/image.h"
#include "host.h"

/* what a filter is told of a frame at configure */
typedef struct {
    const float *noise;    /* HDRG_SD_NOISE of the frame as it came; NULL = none */
    const uint8_t *motion; /* HDRG_SD_MOTION of the frame (checked); NULL = none */
    size_t motion_size;
    const double *xy; /* its color (hdrg_color_describe); NULL = none */
    int64_t pts;
    uint32_t transfer, bits;
    uint32_t index; /* in the stream (a session's start counts) */
    bool first;     /* the session's first frame (a stage without a ring) */
    bool bypass;    /* the host sheds the stage: passed through, unless it has a window */
} hdrg_stage_frame_t;

#define HDRG_STAGE_PHASES 256u /* INTERP in flight: an interval's phases a job keeps at most */

/*
 * INTERP's job: the frames t - past .. t + future (t: the job's frame, at
 * frames[past]; t + 1 the next one, future at least 1), of the same size,
 * their indices following frame->index; those of t's shot each way; the
 * output's phase and every phase of the interval's outputs
 */
typedef struct {
    const hdrg_source_t *frames[2u * HDRG_LIMIT_INTERP + 2u];
    const uint32_t *phases;
    uint32_t past, future, shot_past, shot_future, phase, phase_count;
} hdrg_stage_between_t;

/* memory of the stage: data aligned to HDRG_FORMAT_PAGE within block */
typedef struct {
    void *block;
    uint8_t *data;
    size_t bytes; /* data holds */
} hdrg_stage_buffer_t;

/* a whole frame the filter's jobs see: the host's buffer, or the filter's own (frame_alloc) */
typedef struct {
    hdrg_stage_buffer_t buffer;
    hdrg_format_frame_t view;
    void *handle; /* the filter's; NULL: the host's */
    bool ready;
} hdrg_stage_whole_t;

/* an ASYNC filter's job (a video session keeps max_inflight of them in flight) */
typedef struct {
    hdrg_job_t job;
    hdrg_job_end_t end;
    hdrg_stage_whole_t output;
    const hdrg_format_frame_t *window[2u * HDRG_LIMIT_WINDOW_MAX + 1u]; /* the job's window */
    void *handles[2u * HDRG_LIMIT_WINDOW_MAX + 1u];
    const hdrg_format_frame_t *passed; /* the input passed through (in_layout); NULL: output */
    uint32_t index;                    /* the frame's (the caller's count) */
    bool submitted;                    /* a job not ended yet */
} hdrg_stage_flight_t;

/*
 * A stage of filters (SCALE: to the requested size; PREFILTER: at the
 * source size): one filter instance, created once for a conversion or a
 * session, configured before each frame and run over the source, handing
 * out RGB16 output rows band by band. The filter's jobs see their frames in
 * the formats it declared (filter_set_input, nego_set_output), converted
 * from and to RGB16, and whole frames in its own memory when it allocates
 * them (frame_alloc). Bands may have any height; the input rows a band
 * needs are converted once and each job of HDRG_EXEC_GRAIN_ROWS output rows
 * sees exactly the rows it declared. Filters without BANDS run once over
 * the whole frame at the frame's first rows. Configure only negotiates: a
 * filter that passes the frame through (passthrough) is never processed,
 * its rows are the source's. The source may change between configure and
 * the first rows (a chain's frame before the stage). The frames and buffers
 * stay from frame to frame (no allocation once they fit). An ASYNC filter's
 * job is submitted and waited for (job_done, from any thread).
 *
 * In a video session, a filter with a time window or a clip reads the
 * frames of the stage's ring (in its input format): each input frame is
 * pushed into it, and the frame taken next (whose future has come, or the
 * stream ended) is the jobs' input with its neighbors as their window
 * (hdrg_stage_taken: the frame as RGB16, the stage's source). An ASYNC
 * filter with max_inflight above 1 has its frames submitted
 * (hdrg_stage_submit) and landed (hdrg_stage_land) in order, up to
 * max_inflight in flight. The ring holds past + future + max_inflight
 * frames, or what the caller reserves (a SCALE stage taking frames behind a
 * session's queue). Without the ring (a still) a job has its own frame
 * alone. A frame one stage landed goes into the next one's ring without a
 * copy when both make their frames alike (hdrg_stage_push).
 *
 * SCALE: a filter without BANDS may give a size of its own (a model's fixed
 * factor) instead of the requested one; its whole output is then fitted to
 * the requested size by the built-in Lanczos (fit, a stage of its own).
 */
typedef struct hdrg_stage {
    hdrg_filter_t filter;
    const hdrg_class_t *cls;
    const hdrg_source_t *src;
    const hdrg_allocator_t *alloc;
    void *inst;
    hdrg_stage_flight_t *flights;      /* ASYNC: max_inflight of them (a waited job: the first) */
    hdrg_stage_whole_t *ring;          /* ring_frames of in_layout */
    int64_t *ring_pts;                 /* ... their pts */
    uint32_t *ring_index;              /* INTERP: the frame each slot holds (UINT32_MAX: none) */
    uint8_t *ring_cut;                 /* a scene cut before each frame (hdrg_stage_cut) */
    const hdrg_stage_whole_t *current; /* the taken frame (the ring's), the jobs' input */
    hdrg_stage_whole_t *landed; /* the flight's output last landed, as it is (RGB16); NULL: none */
    const hdrg_format_frame_t *window[2u * HDRG_LIMIT_WINDOW_MAX + 1u]; /* ... t + k at past + k */
    void *handles[2u * HDRG_LIMIT_WINDOW_MAX + 1u];
    const uint16_t *done_rgb; /* the frame's output rows as RGB16 (else from output) */
    void *device_in;          /* the input on the device: the previous filter's frame */
    void *device_frame;       /* the output it gave on the device (until released) */
    void **device_ring;       /* a session's frames on the device (the caller's), ring_frames */
    void *device_window[2u * HDRG_LIMIT_WINDOW_MAX + 1u]; /* ... the taken one's, as window */
    hdrg_status_t *results;
    hdrg_job_scratch_t *scratches; /* the workers' job_scratch, kept across the frames ... */
    size_t *asked;                 /* ... what each chunk's job asked for, ... */
    size_t scratch_want;           /* ... the most of them: every worker's grown to it */
    uint32_t scratch_count;        /* ... (a band's workers: hdrg_exec_concurrency) */
    hdrg_stage_whole_t input;      /* a whole frame's input (without the ring), input format */
    hdrg_stage_whole_t output;     /* whole output of a filter without BANDS */
    hdrg_stage_buffer_t in_rows;   /* a band's input rows, input format */
    hdrg_stage_buffer_t in16;      /* ... as RGB16 first (another input format) */
    hdrg_stage_buffer_t out_rows;  /* a band's output rows (another output format than RGB16) */
    hdrg_stage_buffer_t frame16;   /* an output as RGB16 (4:2:0, a landed frame) */
    hdrg_stage_buffer_t taken16;   /* the taken frame as RGB16 (another input format) */
    hdrg_stage_buffer_t scratch;   /* the 4:2:0 conversions' */
    hdrg_job_side_t side;          /* INTERP: the output's side data */
    hdrg_job_side_t *sides; /* INTERP in flight: each flight's (sides_ready locks made) ... */
    uint32_t *phase_store;  /* ... and its phases (HDRG_STAGE_PHASES each) */
    uint32_t sides_ready;
    hdrg_format_layout_t in_layout, out_layout; /* a whole input / output frame's */
    size_t done_stride;
    int64_t pts, taken_pts; /* the configured frame's, the taken one's */
    uint64_t device_in_ready, device_ready;
    uint32_t ring_frames, rings_ready, reserved; /* reserved: hdrg_stage_reserve's */
    uint32_t out_width, out_height, out_format, halo, max_inflight; /* out: the filter's */
    uint32_t req_width, req_height; /* SCALE: the size asked for (fitting: not the filter's) */
    uint32_t max_band_rows;
    uint32_t past, future, clip, overlap; /* the filter's window (a clip's: clip - 1 + overlap) */
    uint32_t index, flags;                /* the configured frame's in the stream, HDRG_JOB_* */
    uint32_t device;                      /* the filter's kind (filter_set_device) */
    uint32_t taken_flags;                 /* ... of the taken frame */
    uint32_t pushed, taken;               /* frames into the ring and out of it */
    uint32_t have_past, have_future;      /* the taken frame's neighbors in the ring */
    uint32_t shot_past, shot_future;      /* ... those in its shot (shots) */
    uint32_t flight_head, flight_count, flights_ready; /* ready: their ends initialized */
    bool created, configured, passthrough, done;       /* done: the frame's output is there */
    bool side_ready;                                   /* side's lock initialized */
    bool shots;                                        /* the caller marks the cuts */
    bool device_out;               /* the output goes to the next filter on the device */
    bool fitting;                  /* SCALE: the filter's size fitted to the requested one ... */
    bool fit_ready;                /* ... this frame's fit configured */
    struct hdrg_stage *fit;        /* ... by this stage of hdrg.scale.lanczos (made at the first) */
    hdrg_filter_entry_t fit_entry; /* ... of these options */
    hdrg_options_t *fit_options;
    hdrg_source_t fit_src; /* ... over the filter's output as RGB16 */
} hdrg_stage_t;

/*
 * the filter's instance; max_band_rows bounds the rows of one hdrg_stage_rows
 * call. HDRG_ERR_UNSUPPORTED when its create said so (a GPU filter without a
 * device), HDRG_ERR_PLUGIN for any other failure of the filter
 */
extern hdrg_status_t hdrg_stage_create(const hdrg_allocator_t *alloc,
                                       const hdrg_filter_entry_t *entry, uint32_t max_band_rows,
                                       hdrg_plugin_host_t *host, hdrg_stage_t *out);
/* before each frame: src (the stage's input, RGB16 or the caller's) to out_width x out_height */
extern hdrg_status_t hdrg_stage_configure(hdrg_stage_t *stage, const hdrg_source_t *src,
                                          uint32_t out_width, uint32_t out_height,
                                          const hdrg_stage_frame_t *frame,
                                          hdrg_plugin_host_t *host);
/*
 * A frame (src: the stage's input, its pts) into the ring of a stage with a
 * time window or frames in flight. When src is the frame from last landed
 * (hdrg_stage_land; from NULL: none) and both stages make their frames
 * alike (the host's of one allocator, or the filters' of one frame_alloc and
 * frame_free, in the same layout), the frame itself goes in and the ring
 * slot's frame takes its place in from's flight; otherwise src is copied.
 */
extern hdrg_status_t hdrg_stage_push(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                     const hdrg_source_t *src, int64_t pts, hdrg_stage_t *from);
/* a scene cut before the frame pushed last (a session's SCALE: the jobs' shots, job_shot) */
extern void hdrg_stage_cut(hdrg_stage_t *stage, bool cut);
/*
 * A session's frame on the device (a frame of the library's, the caller's:
 * kept by it until the frame leaves the window, ring_frames pushes later)
 * into the ring of a stage with a time window: the taken frame's window is
 * then on the device (device_window: the input at past, the caller's
 * device_in once configured; job_device_window)
 */
extern hdrg_status_t hdrg_stage_push_device(hdrg_stage_t *stage, void *frame, int64_t pts);
/*
 * The ring slot the next frame (of width x height) goes into, made: *rgb and
 * *stride, to write the frame there before hdrg_stage_push gets it (src's
 * pixels and stride these: not copied). HDRG_ERR_UNSUPPORTED when the input
 * format is not RGB16.
 */
extern hdrg_status_t hdrg_stage_intake(hdrg_stage_t *stage, uint32_t width, uint32_t height,
                                       uint16_t **rgb, size_t *stride);
/*
 * The ring's next frame when its future has come (or ended: the input
 * ended, with fewer); false when it waits. The frames around it (within its
 * clip, the stream's clips counted from origin: the index of the ring's
 * first frame) are the jobs' window until the next push.
 */
extern bool hdrg_stage_take(hdrg_stage_t *stage, uint32_t origin, bool ended);
/* the HDRG_SD_MOTION the stage's last job gave (NULL: none), *size its bytes */
extern const uint8_t *hdrg_stage_motion(const hdrg_stage_t *stage, size_t *size);
/* the taken frame as RGB16: *rgb (rows of *stride bytes) */
extern hdrg_status_t hdrg_stage_taken(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                      const uint16_t **rgb, size_t *stride);
/* the configured frame's job run without rows (a SEQUENTIAL filter's frame not shown) */
extern hdrg_status_t hdrg_stage_settle(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                       hdrg_plugin_host_t *host);
/* the device frame the filter gave back to it (after the next filter's job) */
extern void hdrg_stage_device_release(hdrg_stage_t *stage);
/*
 * An ASYNC filter's frames: at most most in flight (1: each job waited for at
 * once) and the ring as much smaller, never below the reserve (before the first push)
 */
extern void hdrg_stage_cap_flights(hdrg_stage_t *stage, uint32_t most);
/* the ring holds at least frames (before the first push) */
extern void hdrg_stage_reserve(hdrg_stage_t *stage, uint32_t frames);
/* the frame's rows come from rgb (a landed frame, rows of stride bytes) */
extern void hdrg_stage_serve(hdrg_stage_t *stage, const uint16_t *rgb, size_t stride);
/* the taken frame, configured, into flight as frame `index`; it ends later */
extern hdrg_status_t hdrg_stage_submit(hdrg_stage_t *stage, uint32_t index,
                                       hdrg_plugin_host_t *host);
/*
 * the earliest frame in flight, waited for: its output as RGB16 and its
 * index (landed: the flight's output, when that is the RGB16 itself)
 */
extern hdrg_status_t hdrg_stage_land(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                     const uint16_t **rgb, size_t *stride, uint32_t *index,
                                     hdrg_plugin_host_t *host);
/*
 * The frame last landed (hdrg_stage_land), when it is at pixels, given to
 * the caller in *spare: spare's frame (one the stage gave before, or none)
 * takes its place in the flight; false: nothing given (passed through,
 * another format, not at pixels)
 */
extern bool hdrg_stage_detach(hdrg_stage_t *stage, const uint8_t *pixels,
                              hdrg_stage_whole_t *spare);
/* a frame the stage (or one making its frames alike, hdrg_stage_push) gave, released */
extern void hdrg_stage_release(hdrg_stage_t *stage, hdrg_stage_whole_t *whole);
/*
 * INTERP: the frame at between's phase (in 1/65536 of the interval) between
 * its frames t (the job's, configured with frame) and t + 1, the window
 * around them (within the stage's past and future, the filter's): the
 * output as RGB16 at *rgb, rows of *stride bytes, and the HDRG_SD_MOTION
 * the filter gave with it at *motion (*motion_size bytes; NULL: none), until
 * the stage's next frame. The frames are converted into the ring once each,
 * by index: a frame of an index must be the same frame each time
 */
extern hdrg_status_t hdrg_stage_interp(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                       const hdrg_stage_between_t *between,
                                       const hdrg_stage_frame_t *frame, hdrg_plugin_host_t *host,
                                       const uint16_t **rgb, size_t *stride, const uint8_t **motion,
                                       size_t *motion_size);
/*
 * INTERP in flight (an ASYNC filter): hdrg_stage_interp's job submitted into
 * the next flight (its window's frames stay in the ring until it lands: at
 * most max_inflight intervals in flight). The phases at most
 * HDRG_STAGE_PHASES
 */
extern hdrg_status_t hdrg_stage_interp_submit(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                              const hdrg_stage_between_t *between,
                                              const hdrg_stage_frame_t *frame,
                                              hdrg_plugin_host_t *host);
/* ... the earliest one waited for: its output and HDRG_SD_MOTION as hdrg_stage_interp's */
extern hdrg_status_t hdrg_stage_interp_land(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                            hdrg_plugin_host_t *host, const uint16_t **rgb,
                                            size_t *stride, const uint8_t **motion,
                                            size_t *motion_size);
/* output rows [y0, y0 + rows) into dst (out_width * 3 uint16_t per row) */
extern hdrg_status_t hdrg_stage_rows(hdrg_stage_t *stage, const hdrg_exec_t *exec, uint32_t y0,
                                     uint32_t rows, uint16_t *dst, hdrg_plugin_host_t *host);
/* destroys the instance; a zeroed stage is left alone */
extern void hdrg_stage_close(hdrg_stage_t *stage);

#endif /* HDRG_HOST_STAGE_H */
