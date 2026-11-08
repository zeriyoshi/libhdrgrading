/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading plugin ABI: the API table, the filters (this header) and the
 * codecs (codec.h, which this one includes).
 *
 * A plugin includes only the public headers (this one and those it
 * includes) and never links the library: it calls the host through the API
 * table its entry point receives. Everything the host owns is an opaque
 * handle. The library's internal layers (src/foundation, src/gpu) are not part
 * of the ABI, but for the GPU share (hdrgrading/foundation/gpu_share.h:
 * filter_gpu_share, the frames on the device), versioned on its own
 * (HDRG_GPU_SHARE_VERSION).
 *
 * - Strings always travel as (pointer, byte length). The host validates
 *   them inside the call (length limits, UTF-8, character sets, SPDX syntax),
 *   copies them and never keeps a pointer into plugin memory; invalid input
 *   is rejected, never truncated.
 * - No enums or bools cross the boundary: constants are uint32_t, flags
 *   0 / 1 are uint8_t. The host's objects stay opaque; what a stage hands
 *   over (a light's record, a frame's statistics) is a plain struct of
 *   fixed-size integers, floats and pointers laid out as this header has it.
 * - The ABI is not frozen yet: it stays at HDRG_PLUGIN_ABI_VERSION 1 and
 *   may change in any way until it is. A module must be built against the
 *   library's own header; one reporting another version is rejected at load
 *   before it can touch the table.
 * - Registration functions work only inside the entry point; after it
 *   returns the plugin is frozen (HDRG_ERR_INVALID_STATE).
 */
#ifndef HDRGRADING_FILTER_H
#define HDRGRADING_FILTER_H

#include <stddef.h>
#include <stdint.h>

#include "codec.h"
#include "plugin.h"
#include "sidedata.h"
#include "video.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HDRG_PLUGIN_ABI_VERSION UINT32_C(1)

#if defined(_WIN32)
#define HDRG_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define HDRG_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define HDRG_PLUGIN_EXPORT
#endif

#ifdef __cplusplus
#define HDRG_PLUGIN_EXTERN_C extern "C"
#else
#define HDRG_PLUGIN_EXTERN_C
#endif

/*
 * Pixel formats of a job's frames (filter_set_input, nego_set_output): the
 * source's code values (its transfer and primaries), samples in native byte
 * order. The host converts from and to RGB16, its own (8-bit sources are
 * widened as v * 257). A whole frame's planes (a filter without BANDS, a
 * window's frames) have strides of multiples of 256 bytes from a base
 * aligned to 16 KiB; a band's rows are packed.
 *   RGB16         interleaved R, G, B uint16_t
 *   RGBA16        interleaved R, G, B, A uint16_t (A = 65535 in, ignored out)
 *   RGB16_PLANAR  planes R, G, B of uint16_t
 *   RGBF16_PLANAR planes R, G, B of IEEE 754 half floats, code / 65535
 *                 (out: clamped to [0, 1], NaN = 0)
 *   P016          Y'CbCr 4:2:0: a Y' plane and a CbCr plane of interleaved
 *                 uint16_t pairs, chroma sited at the left (MPEG-2) and of the
 *                 matrix and range filter_set_input declared; the top 10 bits
 *                 are P010. Whole frames only (no BANDS). Chroma row r goes
 *                 with luma rows 2r and 2r + 1
 *   RGBA8         interleaved R, G, B, A uint8_t (rounded; A = 255)
 *   RGB10A2       a uint32_t a pixel: R in bits 0-9, G 10-19, B 20-29, A 30-31
 *                 (DXGI R10G10B10A2_UNORM, MTLPixelFormatRGB10A2Unorm)
 *   RGBF32_PLANAR planes R, G, B of IEEE 754 single floats, code / 65535 (an
 *                 inference runtime's NCHW float32 tensor; out: clamped to
 *                 [0, 1], NaN = 0, rounded to the nearest code)
 */
#define HDRG_FORMAT_RGB16         UINT32_C(1)
#define HDRG_FORMAT_RGBA16        UINT32_C(2)
#define HDRG_FORMAT_RGB16_PLANAR  UINT32_C(3)
#define HDRG_FORMAT_RGBF16_PLANAR UINT32_C(4)
#define HDRG_FORMAT_P016          UINT32_C(5)
#define HDRG_FORMAT_RGBA8         UINT32_C(6)
#define HDRG_FORMAT_RGB10A2       UINT32_C(7)
#define HDRG_FORMAT_RGBF32_PLANAR UINT32_C(8)

/* job_frame flags */
#define HDRG_JOB_FIRST                                                                             \
    UINT32_C(0x01) /* the first frame of the session or of its clip: state resets */

typedef struct hdrg_class hdrg_class_t;
typedef struct hdrg_filter hdrg_filter_t; /* a filter instance in a conversion */
typedef struct hdrg_negotiation hdrg_negotiation_t;
typedef struct hdrg_job hdrg_job_t;
typedef struct hdrg_lights hdrg_lights_t;       /* a frame's judged lights (HDRG_STAGE_ANALYZE) */
typedef struct hdrg_rendering hdrg_rendering_t; /* a frame rendering (HDRG_STAGE_STATS) */

/*
 * HDRG_STAGE_ANALYZE: a judged light of the frame (lights_get), its core's
 * box in the analyzed frame's pixels (lights_frame's), the rules' boost
 * (a share of the headroom, 0..1) and its features, in order: the core's
 * area (log2(1 + its cells) / 8), mean and brightest La, brightest channel
 * and mean chroma (the largest less the smallest channel); its surround's
 * mean La (2 cells around the core's box) and the core's contrast to it; the
 * boost; the frame's key (log2 of the log-average La / 8) and p99.5 of La.
 * La: the analysis' linear luminance of the SDR source (white 1)
 */
#define HDRG_LIGHT_FEATURES 10u
typedef struct {
    uint32_t x, y, width, height;
    float boost;
    float features[HDRG_LIGHT_FEATURES];
} hdrg_light_record_t;

/*
 * HDRG_STAGE_STATS: a video session's statistics across its frames, its
 * temporal mode (hdrg_session_config_t.temporal: WINDOWED with a window,
 * RECURSIVE, TWO_PASS). The session as created (filter_stats_session, from
 * create on): the mode, latency profile and windows (the future resolved),
 * the first frame's number, the frames' size, the frames back its scene-cut
 * test reads, the rate (frames a second) and a TWO_PASS second pass's first
 * pass (NULL: none)
 */
typedef struct {
    uint32_t temporal, latency, past, future, start, width, height, scene_reach;
    float rate;
    const hdrg_session_pass_t *passes;
    uint32_t pass_count;
} hdrg_stats_session_t;
/* a judged light: its core's box in the light planes' cells (inclusive), its boost */
typedef struct {
    uint32_t x0, y0, x1, y1;
    float boost;
} hdrg_stats_light_t;
/*
 * a frame as sent, from its first analysis (fade): its number, the
 * session's scene cut before it, its p99.5 and log-average of La (linear)
 */
typedef struct {
    uint32_t index;
    uint8_t cut;
    float anchor, key;
} hdrg_stats_sent_t;
/*
 * ... what the class makes of it: run, the frames of the darkening run up
 * to it (0: none), and from, the run's first frame; hold, whether it
 * renders with the statistics the frame from rendered with (a fade-out);
 * cut, its scene cut (the session's, or a TWO_PASS second pass's first's);
 * dim, in a run, the dimming its analysis runs again with (the lights'
 * thresholds scaled by it, at most 1)
 */
typedef struct {
    uint32_t run, from;
    uint8_t hold, cut;
    float dim;
} hdrg_stats_fade_t;
/*
 * a frame's analysis (frame at its send, stats_queued at another's
 * render): its number, its cells (width x height, each 2^cell_shift of the
 * frame's pixels), scene cut, p99.5 and log-average La (its first analysis),
 * its cells' mean Y (the first analysis'; NULL: none), and the analysis it
 * renders from (a run's second): per cell the boost of the light it holds or
 * is near, 1 + that light's index (0: none), and its lights
 */
typedef struct {
    uint32_t index, width, height, light_count;
    uint8_t cell_shift, cut;
    float anchor, key;
    const float *luma, *light;
    const uint32_t *light_of;
    const hdrg_stats_light_t *lights;
} hdrg_stats_frame_t;
/*
 * the frame rendering (stats_head): its analysis as a frame's; anchor and
 * key, the statistics it renders with (the class writes them); its cells'
 * and lights' boosts, steadied in place
 */
typedef struct {
    uint32_t index, width, height, light_count;
    uint8_t cell_shift, cut;
    float anchor, key;
    float *light;
    const uint32_t *light_of;
    hdrg_stats_light_t *lights;
} hdrg_stats_head_t;
/*
 * A STATS class's work (all required, besides create and destroy; no
 * configure, process nor submit): the frames, in order, as sent and as each
 * renders (the frames sent after one rendering are queued: stats_queued).
 * window: the frames before a frame its state reads (*past: the preroll of
 * a seek) and the frames after it sent before it renders (*future) under a
 * latency profile (HDRG_LATENCY_*); fade: a frame as sent; frame: its
 * analysis, once final; render: the frame rendering; fit: its APL factor
 * (log2) as planned: *replace and *used the one it renders with instead;
 * rendered: what it rendered with (log2: its own anchor, key and factor
 * before the class's). Deterministic: the session's bytes are a pure function
 * of the frames
 */
typedef void(HDRG_PLUGIN_CALL *hdrg_stats_window_fn_t)(void *inst, uint32_t latency, uint32_t *past,
                                                       uint32_t *future,
                                                       const hdrg_plugin_api_t *api);
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_stats_fade_fn_t)(void *inst,
                                                              const hdrg_stats_sent_t *sent,
                                                              hdrg_stats_fade_t *out,
                                                              const hdrg_plugin_api_t *api);
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_stats_frame_fn_t)(void *inst,
                                                               const hdrg_stats_frame_t *frame,
                                                               const hdrg_plugin_api_t *api);
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_stats_render_fn_t)(void *inst, hdrg_rendering_t *stats,
                                                                const hdrg_plugin_api_t *api);
typedef void(HDRG_PLUGIN_CALL *hdrg_stats_fit_fn_t)(void *inst, uint32_t index, float fit,
                                                    uint8_t *replace, float *used,
                                                    const hdrg_plugin_api_t *api);
typedef void(HDRG_PLUGIN_CALL *hdrg_stats_rendered_fn_t)(void *inst, uint32_t index, float anchor,
                                                         float key, float fit,
                                                         const hdrg_plugin_api_t *api);

/*
 * Callbacks receive the API table as their last argument, so a plugin needs
 * no global state to call the host. An instance lives as long as the
 * conversion or the video session it runs in (create once, then configure
 * before each frame), so it may keep a model, a device or buffers across
 * frames.
 */
/*
 * required: makes the instance state (*inst) from validated options, which
 * are valid only within this call (copy the values the instance keeps)
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_filter_create_fn_t)(hdrg_filter_t *filter,
                                                                 const hdrg_options_t *options,
                                                                 void **inst,
                                                                 const hdrg_plugin_api_t *api);
/* required */
typedef void(HDRG_PLUGIN_CALL *hdrg_filter_destroy_fn_t)(void *inst, const hdrg_plugin_api_t *api);
/* required, before each frame: reads the input with nego_input, declares the output */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_filter_configure_fn_t)(void *inst,
                                                                    hdrg_negotiation_t *nego,
                                                                    const hdrg_plugin_api_t *api);
/* required without ASYNC: one job = output rows of one band (BANDS) or the whole frame */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_filter_process_fn_t)(void *inst, hdrg_job_t *job,
                                                                  const hdrg_plugin_api_t *api);
/*
 * required with ASYNC (a GPU, an inference runtime): starts the whole
 * frame's job and returns; api->job_done(job, status) ends it once, from
 * any thread (a driver's completion callback too: the host only signals in
 * it, never blocks long nor calls the plugin back), the job and its frames
 * valid until then. A failed device still owes every job its job_done (a
 * CUDA host function does not run after a context error: end the jobs from
 * elsewhere). A failure returned here ends the job without job_done. With
 * max_inflight above 1, the next frames' configure and submit come before
 * the job ends: keep what a job needs with it. In builds without threads
 * (WASM without pthreads) the host cannot wait: job_done must come within
 * submit (a job not ended when submit returns fails, and the job is gone)
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_filter_submit_fn_t)(void *inst, hdrg_job_t *job,
                                                                 const hdrg_plugin_api_t *api);
/*
 * required of an ANALYZE class, its work (no configure, process nor submit):
 * per frame with judged lights, the chance each is a light
 * (lights_set_chance) from their records (lights_get) and the frame
 * (lights_frame). The library scales a light's boost by (1 - amount) +
 * amount x chance (amount: hdrg_params_t.light_predictor): a class lowers
 * a doubtful light, never raises one. Deterministic chances keep the
 * library's bytes (integers); a model on a GPU or an NPU matches within its
 * tolerance
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_filter_analyze_fn_t)(void *inst, hdrg_lights_t *lights,
                                                                  const hdrg_plugin_api_t *api);
/*
 * optional: input rows [*in_begin, *in_end) needed for output rows
 * [out_begin, out_end). Must be pure and monotonic (a sub-range never needs
 * rows outside the range's). NULL: scaled range widened by the halo.
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_filter_input_rows_fn_t)(void *inst, uint32_t out_begin,
                                                                     uint32_t out_end,
                                                                     uint32_t *in_begin,
                                                                     uint32_t *in_end,
                                                                     const hdrg_plugin_api_t *api);
/*
 * optional, without BANDS: the whole frames of the filter's jobs (inputs,
 * a window's frames, outputs) in memory of its own, such as an IOSurface's
 * CVPixelBuffer or pinned CUDA host memory: planes and strides (each at
 * least a packed row, both multiples of the format's sample: 2 bytes, 1 for
 * RGBA8, 4 for RGB10A2) of a frame of format and size, and *handle, what the
 * jobs give back (job_input_handle / job_output_handle). The host keeps a
 * frame until it frees it. Classes registering the same alloc and free make
 * frames any of their instances uses and frees: in a session the host hands
 * a frame one such filter output on to the next one's input without a copy,
 * and the frame may outlive the instance that made it (not the plugin)
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_filter_frame_alloc_fn_t)(
    void *inst, uint32_t format, uint32_t width, uint32_t height, uint8_t **planes, size_t *strides,
    void **handle, const hdrg_plugin_api_t *api);
typedef void(HDRG_PLUGIN_CALL *hdrg_filter_frame_free_fn_t)(void *inst, void *handle,
                                                            const hdrg_plugin_api_t *api);
/*
 * optional, ASYNC: end the jobs in flight soon (job_done with their results
 * or a failure). The host calls it before it waits for them when it stops
 * early: a session destroyed or failing with frames in flight
 */
typedef void(HDRG_PLUGIN_CALL *hdrg_filter_cancel_fn_t)(void *inst, const hdrg_plugin_api_t *api);
/* required with filter_set_device: the host is done with a device frame the filter gave */
typedef void(HDRG_PLUGIN_CALL *hdrg_filter_device_release_fn_t)(void *inst, void *frame,
                                                                const hdrg_plugin_api_t *api);
/*
 * optional: releases what the entry point set up, when the plugin is
 * unregistered; user: its plugin_set_user pointer (NULL = none)
 */
typedef void(HDRG_PLUGIN_CALL *hdrg_plugin_deinit_fn_t)(void *user, const hdrg_plugin_api_t *api);

struct hdrg_plugin_api {
    /* --- the plugin (entry point only) --- */
    /* id: reverse domain, [a-z0-9._-]; version: SemVer characters */
    hdrg_status_t(HDRG_PLUGIN_CALL *plugin_set_info)(hdrg_plugin_t *plugin, const char *id,
                                                     size_t id_len, const char *name,
                                                     size_t name_len, const char *version,
                                                     size_t version_len, const char *vendor,
                                                     size_t vendor_len);
    /* expression: SPDX license expression; a LicenseRef- needs its text */
    hdrg_status_t(HDRG_PLUGIN_CALL *plugin_set_license)(hdrg_plugin_t *plugin,
                                                        const char *expression,
                                                        size_t expression_len,
                                                        const char *copyright,
                                                        size_t copyright_len);
    hdrg_status_t(HDRG_PLUGIN_CALL *plugin_add_license_text)(hdrg_plugin_t *plugin,
                                                             const char *spdx_id, size_t id_len,
                                                             const char *text, size_t text_len);
    /* bundled libraries, model weights, data: each with its own license */
    hdrg_status_t(HDRG_PLUGIN_CALL *plugin_add_component)(
        hdrg_plugin_t *plugin, const char *name, size_t name_len, const char *version,
        size_t version_len, const char *expression, size_t expression_len, const char *copyright,
        size_t copyright_len, const char *notice, size_t notice_len);
    hdrg_status_t(HDRG_PLUGIN_CALL *plugin_set_status)(hdrg_plugin_t *plugin, uint32_t status,
                                                       uint32_t reason, const char *message,
                                                       size_t message_len);
    hdrg_status_t(HDRG_PLUGIN_CALL *plugin_set_deinit)(hdrg_plugin_t *plugin,
                                                       hdrg_plugin_deinit_fn_t fn);
    /*
     * a pointer every filter and codec of the plugin reads back (filter_plugin_user, read_users,
     * write_users), such as a device or models shared by its instances. The plugin owns it
     * (release it in deinit) and keeps it safe for concurrent instances
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *plugin_set_user)(hdrg_plugin_t *plugin, void *user);

    /*
     * --- memory (any time, any thread) ---
     * the context's allocator (hdrg_context_desc_t.allocator): api is the table
     * the call received, which names the context. NULL when it fails. An aligned
     * block (align a power of two, at least sizeof(void *)) is freed by
     * mem_aligned_free alone. Free everything before the plugin's deinit returns
     */
    void *(HDRG_PLUGIN_CALL *mem_alloc)(const hdrg_plugin_api_t *api, size_t size);
    void *(HDRG_PLUGIN_CALL *mem_calloc)(const hdrg_plugin_api_t *api, size_t count, size_t size);
    void *(HDRG_PLUGIN_CALL *mem_realloc)(const hdrg_plugin_api_t *api, void *ptr, size_t size);
    void(HDRG_PLUGIN_CALL *mem_free)(const hdrg_plugin_api_t *api, void *ptr);
    void *(HDRG_PLUGIN_CALL *mem_aligned_alloc)(const hdrg_plugin_api_t *api, size_t size,
                                                size_t align);
    void(HDRG_PLUGIN_CALL *mem_aligned_free)(const hdrg_plugin_api_t *api, void *ptr);

    /*
     * --- the context's threads and conversions (any time, any thread) ---
     * workers: how many of a run_parallel's fn calls may run at once (1
     * without threads), the bound of their worker argument
     */
    uint32_t(HDRG_PLUGIN_CALL *run_workers)(const hdrg_plugin_api_t *api);
    /*
     * fn(user, i, worker) for every i in [0, count) on the context's threads
     * (the calling one among them), returning when all ran; each worker runs
     * one at a time. Not from within fn
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *run_parallel)(const hdrg_plugin_api_t *api, uint32_t count,
                                                  hdrg_parallel_fn_t fn, void *user);
    /*
     * 16-bit R'G'B' (the codes x 65535, rows of stride bytes, at least
     * width x 3 samples) of the Y'CbCr picture desc describes (HDRG_YUVD_*
     * words, planes Y', Cb, Cr with strides in bytes): subsampled chroma
     * upsampled bilinearly from its sites, plus the luma's detail where the
     * chroma follows the luma (fit: HDRG_CHROMA_FIT_*, the read_limits one
     * for a reader's picture). A strip of a larger picture converts as the
     * whole one does at rows a few luma rows (8) inside it
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *yuv_to_rgb16)(const hdrg_plugin_api_t *api,
                                                  const uint32_t *desc, const void *const *planes,
                                                  const size_t *strides, uint32_t fit,
                                                  uint16_t *rgb, size_t stride);
    /*
     * rows [y0, y0 + rows) of 16-bit R'G'B' (width x 3 samples a row, rgb's
     * first row the row y0) into the Y'CbCr picture desc describes, its
     * planes written: 4:4:4 each pixel's, 4:2:0 (y0 even) the chroma of
     * each row pair from the [1 2 1] / 4 filter on the even columns and the
     * two rows' mean (siting HDRG_CHROMA_LEFT); 4:2:2 is not written
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *rgb16_to_yuv)(const hdrg_plugin_api_t *api,
                                                  const uint32_t *desc, void *const *planes,
                                                  const size_t *strides, const uint16_t *rgb,
                                                  uint32_t y0, uint32_t rows);

    /*
     * --- classes (entry point only). NULL on error ---
     * a class is a filter (class_set_stages and the calls below) or a codec
     * (class_set_reader / class_set_writer, codec.h), never both; the
     * options and class_register are both kinds'
     */
    hdrg_class_t *(HDRG_PLUGIN_CALL *class_create)(hdrg_plugin_t *plugin, const char *id,
                                                   size_t id_len);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_stages)(hdrg_class_t *cls, uint32_t stages);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_flags)(hdrg_class_t *cls, uint32_t flags);
    /* key: [a-z0-9_] starting with [a-z]; min <= default <= max */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_add_option_int)(hdrg_class_t *cls, const char *key,
                                                          size_t key_len, int64_t def, int64_t min,
                                                          int64_t max, const char *description,
                                                          size_t description_len);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_add_option_float)(hdrg_class_t *cls, const char *key,
                                                            size_t key_len, double def, double min,
                                                            double max, const char *description,
                                                            size_t description_len);
    /*
     * def and every value: UTF-8 without control characters, at most 1 KiB
     * (a model file's path, a mode name)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_add_option_string)(hdrg_class_t *cls, const char *key,
                                                             size_t key_len, const char *def,
                                                             size_t def_len,
                                                             const char *description,
                                                             size_t description_len);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_create)(hdrg_class_t *cls,
                                                      hdrg_filter_create_fn_t fn);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_destroy)(hdrg_class_t *cls,
                                                       hdrg_filter_destroy_fn_t fn);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_configure)(hdrg_class_t *cls,
                                                         hdrg_filter_configure_fn_t fn);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_process)(hdrg_class_t *cls,
                                                       hdrg_filter_process_fn_t fn);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_submit)(hdrg_class_t *cls,
                                                      hdrg_filter_submit_fn_t fn);
    /* an ANALYZE class's analyze */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_analyze)(hdrg_class_t *cls,
                                                       hdrg_filter_analyze_fn_t fn);
    /* a STATS class's work (hdrg_stats_*_fn_t) */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_stats)(
        hdrg_class_t *cls, hdrg_stats_window_fn_t window, hdrg_stats_fade_fn_t fade,
        hdrg_stats_frame_fn_t frame, hdrg_stats_render_fn_t render, hdrg_stats_fit_fn_t fit,
        hdrg_stats_rendered_fn_t rendered);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_cancel)(hdrg_class_t *cls,
                                                      hdrg_filter_cancel_fn_t fn);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_device_release)(hdrg_class_t *cls,
                                                              hdrg_filter_device_release_fn_t fn);
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_input_rows)(hdrg_class_t *cls,
                                                          hdrg_filter_input_rows_fn_t fn);
    /* both or neither; not with BANDS */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_frames)(hdrg_class_t *cls,
                                                      hdrg_filter_frame_alloc_fn_t alloc,
                                                      hdrg_filter_frame_free_fn_t release);
    /*
     * codecs: the file names' extensions, lowercase [a-z0-9] without the dot,
     * comma-separated ("jpg,jpeg"), the first one a written file's
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_extensions)(hdrg_class_t *cls, const char *list,
                                                          size_t list_len);
    /* codecs: reads files (both required) */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_reader)(hdrg_class_t *cls,
                                                      hdrg_codec_probe_fn_t probe,
                                                      hdrg_codec_read_fn_t read);
    /*
     * codecs: writes files (every callback required). forms:
     * HDRG_CODEC_FORM bits; default_form: one of them, the form of
     * HDRG_OUTPUT_AUTO; rows: HDRG_CODEC_ROWS_*; color: HDRG_CODEC_COLOR_*;
     * max_side: a side's pixels at most (1 to 65535)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_writer)(
        hdrg_class_t *cls, uint32_t forms, uint32_t default_form, uint32_t rows, uint32_t color,
        uint32_t max_side, hdrg_codec_create_fn_t create, hdrg_codec_rows_fn_t write_rows,
        hdrg_codec_finish_fn_t finish, hdrg_codec_destroy_fn_t destroy);
    /*
     * either kind: a pointer the class's instances read back (filter_class_user, read_users,
     * write_users), such as the one of several backends a class of the plugin's runs on; the
     * plugin owns it
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_set_user)(hdrg_class_t *cls, void *user);
    /* checks the class (required callbacks, consistency) and freezes it */
    hdrg_status_t(HDRG_PLUGIN_CALL *class_register)(hdrg_class_t *cls);

    /* --- options (create) --- */
    hdrg_status_t(HDRG_PLUGIN_CALL *option_get_int)(const hdrg_options_t *options, const char *key,
                                                    size_t key_len, int64_t *value);
    hdrg_status_t(HDRG_PLUGIN_CALL *option_get_float)(const hdrg_options_t *options,
                                                      const char *key, size_t key_len,
                                                      double *value);
    /*
     * the value copied into buf (cap bytes) with a terminating NUL; needed
     * receives its size with the NUL, and nothing is written when cap is
     * smaller
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *option_get_string)(const hdrg_options_t *options,
                                                       const char *key, size_t key_len, char *buf,
                                                       size_t cap, size_t *needed);

    /* --- the filter instance --- */
    /* any callback: the user pointer of the filter's plugin (NULL = none) */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_plugin_user)(const hdrg_filter_t *filter, void **user);
    /* any callback: the user pointer of the filter's class (class_set_user; NULL = none) */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_class_user)(const hdrg_filter_t *filter, void **user);
    /*
     * any callback: the context's device and queue of kind (HDRG_DEVICE_* of
     * gpu.h): the app's (hdrg_context_set_device), else for METAL, VULKAN
     * and WEBGPU in a build with them the library's own, made on the first
     * ask and shared by the context's filters and its core (WEBGPU: no
     * queue, the device's); HDRG_ERR_NOT_FOUND when there is none
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_device)(const hdrg_filter_t *filter, uint32_t kind,
                                                   void **device, void **queue);
    /*
     * any callback: the context's device of kind as the library and every
     * plugin share it, in share (an hdrg_gpu_share_t of
     * hdrgrading/foundation/gpu_share.h, its struct_size and version set by
     * the asker): the native device and queue, the queue's lock (VULKAN) and
     * the device's thread (WEBGPU), valid until the plugin's deinit.
     * HDRG_ERR_UNSUPPORTED: another version, or no device of the kind
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_gpu_share)(const hdrg_filter_t *filter, uint32_t kind,
                                                      void *share);
    /*
     * create, without BANDS: the jobs can take their input and give their
     * output as frames of the filter's own on a device of kind, kept there
     * between it and a neighboring working filter of the chain that declared
     * the same kind (job_device). A filter with a clip or max_inflight above
     * 1 always gets host frames, one with a window too unless the library
     * runs the chain on its own device frames (a session's frames kept on
     * its GPU, the library's GPU filters alone): then the window's frames
     * are on the device as well (job_device_window)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_set_device)(hdrg_filter_t *filter, uint32_t kind);
    /*
     * create: the format of the job's input frames (default RGB16); matrix
     * (H.273: 1, 5, 6 or 9) and full_range (0 / 1) code P016, in and out
     * (default BT.709, limited range)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_set_input)(hdrg_filter_t *filter, uint32_t format,
                                                      uint32_t matrix, uint8_t full_range);
    /*
     * create: the frames t - past .. t + future a job of frame t reads (each
     * at most 8 in PREFILTER, 32 in SCALE; default 0 / 0). INTERP: its job's
     * frame t and the next one t + 1 the output lies between, and up to 4
     * frames before t and after t + 1 (past at most 4, future 1 to 5;
     * default 0 / 1) for a model reading more context. A video session
     * keeps them for the filter and delays its frames by future; a still's
     * job has its own frame alone. A job's output must depend on its
     * window's frames alone (a cache keyed by the window keeps that: the
     * index and the frames before and after): the host may skip jobs (a
     * seek's preroll) but for SEQUENTIAL filters
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_set_window)(hdrg_filter_t *filter, uint32_t past,
                                                       uint32_t future);
    /*
     * create, instead of a window: the stream is cut into clips of frames
     * frames from its first frame, and a job's window is its frame's clip
     * and overlap frames more on each side (as much of them as the session
     * has; frames - 1 + overlap at most 8 in PREFILTER, 32 in SCALE; frames
     * at least 2; not in INTERP). For bidirectional recurrent models
     * (BasicVSR), the overlap hiding the seams; the delay is frames - 1 +
     * overlap. A SEQUENTIAL filter's jobs come in order, none skipped, the
     * clip's first with HDRG_JOB_FIRST
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_set_clip)(hdrg_filter_t *filter, uint32_t frames,
                                                     uint32_t overlap);
    /*
     * create, ASYNC (required: a filter not calling it fails to create):
     * the frames a video session may have submitted and not seen end at
     * once (1 to 8; 1: each waited for at once). Above 1 a session of
     * HDRG_LATENCY_PLAYBACK (and OFFLINE) delays its frames by frames - 1 to
     * keep them in flight (INTERP: the jobs of the outputs ahead, each with
     * the window it would have alone, submitted once that window's frames
     * are there);
     * HDRG_LATENCY_LOW waits for each at once
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_set_max_inflight)(hdrg_filter_t *filter,
                                                             uint32_t frames);

    /* --- negotiation (configure only) --- */
    /* the input's size and the format the jobs see it in (filter_set_input) */
    hdrg_status_t(HDRG_PLUGIN_CALL *nego_input)(const hdrg_negotiation_t *nego, uint32_t *width,
                                                uint32_t *height, uint32_t *format);
    /*
     * the color of the input's code values: the primaries and the white as
     * CIE xy {rx, ry, gx, gy, bx, by, wx, wy}, the transfer (HDRG_TRANSFER_*
     * of hdrgrading.h, 2 = another curve) and the bits per sample of the
     * source before they were widened to 16
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *nego_input_color)(const hdrg_negotiation_t *nego, double *xy,
                                                      uint32_t *transfer, uint32_t *bits);
    /*
     * the input frame's side data of type copied into buf (cap bytes);
     * version and needed receive its version and size, and nothing is
     * written when cap is smaller. HDRG_ERR_NOT_FOUND when the frame has
     * none of that type
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *nego_side_data)(const hdrg_negotiation_t *nego, uint32_t type,
                                                    uint32_t *version, void *buf, size_t cap,
                                                    size_t *needed);
    /*
     * SCALE: the size the host asks for. A filter without BANDS may give
     * another (nego_set_output: a model's fixed factor, 2x or 4x): the host
     * fits its whole output to the size asked for with its Lanczos
     * (hdrg.scale.lanczos)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *nego_requested_size)(const hdrg_negotiation_t *nego,
                                                         uint32_t *width, uint32_t *height);
    /* any HDRG_FORMAT_* (P016 without BANDS) */
    hdrg_status_t(HDRG_PLUGIN_CALL *nego_set_output)(hdrg_negotiation_t *nego, uint32_t width,
                                                     uint32_t height, uint32_t format);
    /* input rows needed beyond the scaled range on each side */
    hdrg_status_t(HDRG_PLUGIN_CALL *nego_set_halo)(hdrg_negotiation_t *nego, uint32_t rows);
    /*
     * the filter leaves this frame as it is (its side data shows nothing to
     * do). The output size must be the input's; the host then passes the
     * frame on without a job (none is made for it) or drops the stage
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *nego_set_passthrough)(hdrg_negotiation_t *nego);

    /* --- jobs (process / submit, any thread). Call once per job, not per pixel --- */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_rows)(const hdrg_job_t *job, uint32_t *begin,
                                              uint32_t *end);
    /*
     * data points at the plane's row of input row *row_begin (a 4:2:0 chroma
     * plane: row *row_begin / 2); rows [*row_begin, *row_end) are readable
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_input)(const hdrg_job_t *job, uint32_t plane,
                                               const uint8_t **data, size_t *stride,
                                               uint32_t *row_begin, uint32_t *row_end);
    /*
     * the job's frame: its index in the stream (a session's start counts; a
     * still: 0) and the frames before and after it the job may read (fewer
     * at the stream's ends, a clip's at its own)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_window)(const hdrg_job_t *job, uint32_t *index,
                                                uint32_t *past, uint32_t *future);
    /*
     * of the frames job_window gives, those before and after the job's frame
     * in its shot (no scene cut between them and it): a multi-frame model
     * reading across a cut mixes two shots. A session's SCALE and INTERP jobs
     * (INTERP's frame t + 1 always: the host does not interpolate across a
     * cut) and a still's (0, 0); HDRG_ERR_NOT_FOUND in PREFILTER, whose
     * frames come before the cuts are judged
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_shot)(const hdrg_job_t *job, uint32_t *past,
                                              uint32_t *future);
    /* the job's frame: its pts (a session's frame's; a still: 0) and HDRG_JOB_* flags */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_frame)(const hdrg_job_t *job, int64_t *pts,
                                               uint32_t *flags);
    /*
     * INTERP: where the output lies between the job's frame (job_input, 0)
     * and the next one (job_input_frame offset 1, 65536), in 1/65536 of
     * the interval; 0 on the other stages
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_phase)(const hdrg_job_t *job, uint32_t *phase);
    /*
     * INTERP: the phases of every output the session makes between the job's
     * frame and the next one, ascending (job_phase's among them): *count of
     * them, the first cap written to phases (NULL with cap 0: the count). A
     * model making them together (a generative one's sequence) keeps the
     * others for their jobs, keyed by the frame's index and phase, but makes
     * any it is asked for alone too: the host may skip jobs (a seek's
     * preroll). Other stages: 0 phases
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_phases)(const hdrg_job_t *job, uint32_t *phases,
                                                uint32_t cap, uint32_t *count);
    /*
     * the output frame's side data of type: version, size bytes (copied,
     * replacing any of the type the job gave). HDRG_SD_MOTION (checked:
     * HDRG_ERR_INVALID_ARG when its layout is not) of INTERP's jobs and of
     * PREFILTER's whole-frame jobs (not BANDS, at most a frame in flight);
     * other stages and types HDRG_ERR_UNSUPPORTED (not yet). Before
     * job_done, from any thread
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_set_side_data)(hdrg_job_t *job, uint32_t type,
                                                       uint32_t version, const void *data,
                                                       size_t size);
    /* job_input's rows of frame t + offset (offset 0: job_input's own) */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_input_frame)(const hdrg_job_t *job, int32_t offset,
                                                     uint32_t plane, const uint8_t **data,
                                                     size_t *stride, uint32_t *row_begin,
                                                     uint32_t *row_end);
    /*
     * data points at the plane's row of output row begin of job_rows; only
     * the job's rows are writable
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_output)(const hdrg_job_t *job, uint32_t plane,
                                                uint8_t **data, size_t *stride);
    /*
     * the job's scratch memory: size bytes at *data, aligned to 64 bytes, its
     * contents undefined, valid until the job returns. Only process's jobs
     * (HDRG_ERR_UNSUPPORTED in submit's, which outlive the call). The host
     * keeps it for the worker that runs the job and grows it when a job asks
     * for more, so a filter's jobs allocate nothing once their size is
     * reached. One block a job: a later call replaces an earlier one's.
     * HDRG_ERR_NOMEM when it cannot grow
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_scratch)(const hdrg_job_t *job, size_t size, void **data);
    /*
     * the frame_alloc handle of input frame t + offset (offset 0: the job's
     * own) / of the output; NULL without frame_alloc
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_input_handle)(const hdrg_job_t *job, int32_t offset,
                                                      void **handle);
    hdrg_status_t(HDRG_PLUGIN_CALL *job_output_handle)(const hdrg_job_t *job, void **handle);
    /*
     * the job's frames on the device (hdrg_gpu_share_frame_t of
     * hdrgrading/foundation/gpu_share.h for HDRG_FILTER_GPU_FRAMES; else
     * what the filters of the kind agree on): *in, the previous filter's (and
     * *ready, the value it gave with it), NULL when the input is the host's
     * (job_input; HDRG_ERR_NOT_FOUND when it is on the device); *out_wanted
     * 1 when the output goes to the next filter on the device
     * (job_set_output_device; job_output has no planes then)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_device)(const hdrg_job_t *job, void **in, uint64_t *ready,
                                                uint8_t *out_wanted);
    /*
     * frame t + offset of the job's window on the device (offset 0:
     * job_device's input), an hdrg_gpu_share_frame_t; *frame NULL when the
     * window is the host's (job_input_frame). HDRG_ERR_NOT_FOUND outside the
     * window (job_window)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_device_window)(const hdrg_job_t *job, int32_t offset,
                                                       void **frame);
    /* the output as a device frame of the filter's own (released with device_release) */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_set_output_device)(hdrg_job_t *job, void *frame,
                                                           uint64_t ready);
    /* any thread, once per submitted job: its end (HDRG_OK or the failure) */
    hdrg_status_t(HDRG_PLUGIN_CALL *job_done)(hdrg_job_t *job, hdrg_status_t status);

    /* --- analyzing (an ANALYZE class's analyze) --- */
    /* the frame's judged lights (*count of them: 1 or more) */
    hdrg_status_t(HDRG_PLUGIN_CALL *lights_get)(const hdrg_lights_t *lights,
                                                const hdrg_light_record_t **records,
                                                uint32_t *count);
    /*
     * the analyzed frame (a session's analysis proxy too): interleaved 16-bit
     * R'G'B' (the codes x 65535 / their largest, an alpha dropped), rows of
     * *stride bytes, made on the first ask and valid within analyze;
     * HDRG_ERR_NOT_FOUND where the frame is on the GPU alone
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *lights_frame)(hdrg_lights_t *lights, const uint16_t **rgb,
                                                  size_t *stride, uint32_t *width,
                                                  uint32_t *height);
    /* light index's chance, 0..1 (one not set: 1, as judged) */
    hdrg_status_t(HDRG_PLUGIN_CALL *lights_set_chance)(hdrg_lights_t *lights, uint32_t index,
                                                       float chance);

    /* --- a session's statistics (a STATS class's work) --- */
    /* create and after: the session the filter runs in (hdrg_stats_session_t, the session's) */
    hdrg_status_t(HDRG_PLUGIN_CALL *filter_stats_session)(const hdrg_filter_t *filter,
                                                          const hdrg_stats_session_t **session);
    /* render: the frame rendering */
    hdrg_status_t(HDRG_PLUGIN_CALL *stats_head)(hdrg_rendering_t *stats, hdrg_stats_head_t **head);
    /*
     * render: the k-th frame queued after it (1 the next), valid within the
     * render; HDRG_ERR_NOT_FOUND past the queue
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *stats_queued)(hdrg_rendering_t *stats, uint32_t k,
                                                  const hdrg_stats_frame_t **frame);

    /* --- reading (a reader's read) --- */
    /* the pointers of the reader's plugin (plugin_set_user) and class (class_set_user); NULL: skip
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *read_users)(const hdrg_reader_t *reader, void **plugin_user,
                                                void **class_user);
    /* the context's max_pixels and the conversion's chroma fit (HDRG_CHROMA_FIT_*) */
    hdrg_status_t(HDRG_PLUGIN_CALL *read_limits)(const hdrg_reader_t *reader, uint64_t *max_pixels,
                                                 uint32_t *chroma_fit);
    /*
     * once: the picture's buffer of interleaved R'G'B', width x height,
     * samples of bits 8 or 16 (native order), rows of *stride bytes;
     * HDRG_ERR_LIMIT above max_pixels before anything is allocated
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *read_image)(hdrg_reader_t *reader, uint32_t width,
                                                uint32_t height, uint32_t bits, uint8_t **pixels,
                                                size_t *stride);
    /*
     * the color of the code values: H.273 primaries and transfer
     * (HDRG_PRIMARIES_* / HDRG_TRANSFER_*); HDRG_ERR_UNSUPPORTED for a pair
     * the library does not take (the color stays unset)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *read_set_cicp)(hdrg_reader_t *reader, uint32_t primaries,
                                                   uint32_t transfer);
    /*
     * the color as chromaticities and a curve: the primaries and the white
     * {rx, ry, gx, gy, bx, by, wx, wy} (NULL: sRGB's), a pure gamma (the
     * decoding exponent; 0: the sRGB curve); HDRG_ERR_INVALID_ARG for unusable
     * ones (out of range, in a line, a white outside), the color then unset
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *read_set_xy)(hdrg_reader_t *reader, const double *xy,
                                                 double gamma);
    /* an ICC profile (copied): the color it describes wins over read_set_cicp / xy */
    hdrg_status_t(HDRG_PLUGIN_CALL *read_set_icc)(hdrg_reader_t *reader, const uint8_t *icc,
                                                  size_t size);
    /*
     * the Exif (copied; TIFF, without "Exif\0\0"): its orientation unless
     * read_set_orientation, its DCF Adobe RGB when no color is set; the
     * output carries it with the orientation 1
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *read_set_exif)(hdrg_reader_t *reader, const uint8_t *tiff,
                                                   size_t size);
    /* the EXIF orientation (1 to 8) of the picture as decoded: the host turns it upright */
    hdrg_status_t(HDRG_PLUGIN_CALL *read_set_orientation)(hdrg_reader_t *reader,
                                                          uint32_t orientation);

    /* --- writing (a writer's create, rows and finish) --- */
    /* the pointers of the writer's plugin (plugin_set_user) and class (class_set_user); NULL: skip
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_users)(const hdrg_writer_t *writer, void **plugin_user,
                                                 void **class_user);
    /*
     * the file: its size, form (HDRG_OUTPUT_GAINMAP / HYBRID / PQ), the
     * base's color as H.273 code points (primaries 1 sRGB, 12 Display P3, 9
     * BT.2020; transfer 13 sRGB, 16 PQ), the threads the writer may use
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_image)(const hdrg_writer_t *writer, uint32_t *width,
                                                 uint32_t *height, uint32_t *form,
                                                 uint32_t *primaries, uint32_t *transfer,
                                                 uint32_t *threads);
    /* the Exif to carry (TIFF, its orientation 1); HDRG_ERR_NOT_FOUND: none */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_exif)(const hdrg_writer_t *writer, const uint8_t **tiff,
                                                size_t *size);
    /*
     * HDRG_CODEC_COLOR_ICC: the base's profile copied into buf (cap bytes;
     * needed receives its size, nothing is written when cap is smaller):
     * Display P3 for a P3 SDR base, "P3 PQ" with its cicp tag for a PQ one
     * (hagc 1: with the ST 2094-50 curve back to the SDR original in its
     * 'HAGC' tag, finish only; lumi_nits > 0: a luminance tag).
     * HDRG_ERR_NOT_FOUND for an sRGB base (no profile)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_icc)(const hdrg_writer_t *writer, uint32_t hagc,
                                               double lumi_nits, uint8_t *buf, size_t cap,
                                               size_t *needed);
    /*
     * finish, GAINMAP and HYBRID (else HDRG_ERR_NOT_FOUND): the 8-bit
     * monochrome gain map (rows of *stride bytes) and its HDRG_GAINMAP_VALUES
     * values; the alternate rendition's color (H.273 primaries, transfer)
     * and bits
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_gainmap)(const hdrg_writer_t *writer,
                                                   const uint8_t **pixels, size_t *stride,
                                                   uint32_t *width, uint32_t *height,
                                                   double *values);
    hdrg_status_t(HDRG_PLUGIN_CALL *write_alternate)(const hdrg_writer_t *writer,
                                                     uint32_t *primaries, uint32_t *transfer,
                                                     uint32_t *bits);
    /*
     * finish: the gain map's ISO 21496-1 metadata (its GainMapMetadata, the
     * versions first, one channel, the base's color space) into buf, as
     * write_icc
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_iso21496)(const hdrg_writer_t *writer, uint8_t *buf,
                                                    size_t cap, size_t *needed);
    /*
     * finish, GAINMAP: Apple's HDRGainMap XMP packet (its headroom the
     * alternate's) into buf, as write_icc
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_apple_xmp)(const hdrg_writer_t *writer, uint8_t *buf,
                                                     size_t cap, size_t *needed);
    /*
     * finish, HYBRID and PQ (else 0 and 0): the PQ base's measured MaxCLL and
     * MaxFALL (cd/m^2); peak: the rendition's ceiling (the mastering
     * display's maximum)
     */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_light)(const hdrg_writer_t *writer, uint32_t *max_cll,
                                                 uint32_t *max_fall, uint32_t *peak);
    /* finish: appends bytes to the file (copied) */
    hdrg_status_t(HDRG_PLUGIN_CALL *write_output)(hdrg_writer_t *writer, const uint8_t *data,
                                                  size_t size);
};

/*
 * Defines the module exports of a dynamically loaded plugin: the ABI
 * version it was built with (checked before anything else) and the entry
 * point calling init. A plugin linked into the host passes init to
 * hdrg_context_register_plugin instead.
 */
#define HDRG_PLUGIN_DEFINE(init)                                                                   \
    HDRG_PLUGIN_EXTERN_C HDRG_PLUGIN_EXPORT uint32_t HDRG_PLUGIN_CALL hdrg_plugin_abi_version(     \
        void)                                                                                      \
    {                                                                                              \
        return HDRG_PLUGIN_ABI_VERSION;                                                            \
    }                                                                                              \
    HDRG_PLUGIN_EXTERN_C HDRG_PLUGIN_EXPORT hdrg_status_t HDRG_PLUGIN_CALL hdrg_plugin_init(       \
        hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)                                       \
    {                                                                                              \
        return (init)(plugin, api);                                                                \
    }

#ifdef __cplusplus
}
#endif

#endif /* HDRGRADING_FILTER_H */
